#!/usr/bin/env python3
"""Build a personal R1 firmware from the exact compatible platform input.

Vendor/platform binaries stay in the user's input. GeekOS supplies its player,
open fonts, branding, languages and build recipes. This tool never flashes.
"""
import argparse,datetime,hashlib,json,os,re,shlex,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
RELEASE=json.loads(Path(__file__).with_name('firmware.json').read_text())
def pseudo_name(name):
 return '"'+name.replace('\\','\\\\').replace('"','\\"')+'"'
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def tool(name,*fallbacks):
 for candidate in (name,*fallbacks):
  found=shutil.which(candidate)
  if found:return found
 raise SystemExit('Required tool not found: '+name)
def run(args,**kw):return subprocess.run([str(x) for x in args],check=True,**kw)
def unpack(source,workspace,model):
 expected=RELEASE['platform_inputs'][model]
 if digest(source)!=expected['sha256']:raise SystemExit('Platform SHA-256 mismatch. Use the exact '+expected['release']+' input from '+expected['url'])
 iso=workspace/'iso';iso.mkdir()
 run([tool('xorriso'),'-osirrox','on','-indev',source,'-extract','/',iso],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 iso.chmod(0o755)
 for directory in iso.rglob('*'):
  if directory.is_dir():directory.chmod(0o755)
  else:directory.chmod(0o644)
 ota=iso/'ota_v0';chunks=sorted(ota.glob('rootfs.squashfs.[0-9][0-9][0-9][0-9].*'))
 if not chunks:raise SystemExit('Missing root filesystem chunks')
 rootfs=workspace/'platform.squashfs'
 with rootfs.open('wb') as out:
  for chunk in chunks:out.write(chunk.read_bytes())
 return iso,rootfs

def validate_iso(source,workspace,model,player):
 iso=workspace/'verify';iso.mkdir();run([tool('xorriso'),'-osirrox','on','-indev',source,'-extract','/',iso],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 ota=iso/'ota_v0';image_data={}
 for image in ['xImage','rootfs.squashfs']:
  files=sorted(ota.glob(image+'.[0-9][0-9][0-9][0-9].*'));assert files,image
  data=b''.join(p.read_bytes() for p in files);whole=hashlib.md5(data).hexdigest();previous=whole
  hashes=[]
  for index,p in enumerate(files):
   assert p.name==f'{image}.{index:04d}.{previous}'
   previous=hashlib.md5(p.read_bytes()).hexdigest();hashes.append(previous)
  assert (ota/f'ota_md5_{image}.{whole}').read_text().splitlines()==hashes
  image_data[image]=data
 assert hashlib.md5(image_data['xImage']).hexdigest()==RELEASE['platform_inputs'][model]['kernel_md5']
 assert len(image_data['rootfs.squashfs'])<RELEASE['platform_inputs'][model]['rootfs_limit']
 fs=workspace/'verify.squashfs';fs.write_bytes(image_data['rootfs.squashfs'])
 def cat(path):return subprocess.check_output([tool('unsquashfs'),'-cat',str(fs),path])
 assert hashlib.sha256(cat('usr/bin/sonix_player')).hexdigest()==digest(player)
 info=json.loads(cat('usr/resource/sonix/components/system-info.json'))
 assert info['device-name']=='HiBy R1' and info['OS_version']==RELEASE['version'] and info['ota-repo']==RELEASE['repository']
 listing=subprocess.check_output([tool('unsquashfs'),'-ls',str(fs)],text=True)
 assert 'Bookerly-Regular.ttf' in listing and 'inter-regular.ttf' in listing
 assert 'NotoSansDevanagari-Regular.ttf' in listing and 'HarfBuzz.txt' in listing
 return {'model':model,'version':RELEASE['version'],'build_version':info['build_version'],'kernel_md5':RELEASE['platform_inputs'][model]['kernel_md5'],'rootfs_bytes':len(image_data['rootfs.squashfs']),'player_sha256':digest(player),'firmware_sha256':digest(source),'firmware_md5':hashlib.md5(source.read_bytes()).hexdigest()}

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--platform-input',required=True,type=Path);p.add_argument('--player',type=Path,default=ROOT/'sonix-player/sonix_player');p.add_argument('--model',choices=RELEASE['enabled_models'],default='R1');p.add_argument('--output',type=Path,default=ROOT/'dist/r1.upt');args=p.parse_args()
 source=args.platform_input.resolve();player=args.player.resolve();output=args.output.resolve();output.parent.mkdir(parents=True,exist_ok=True)
 epoch=RELEASE['source_date_epoch'];env=os.environ.copy();env.pop('SOURCE_DATE_EPOCH',None)
 with tempfile.TemporaryDirectory(prefix='geekos-firmware-') as scratch:
  work=Path(scratch);iso,fs=unpack(source,work,args.model);pseudo=work/'original.pseudo'
  run([tool('unsquashfs'),'-pf',pseudo,fs],stdout=subprocess.DEVNULL)
  data=pseudo.read_bytes();marker=data.index(b'# START OF DATA - DO NOT MODIFY');head,tail=data[:marker],data[marker:]
  lines={}
  for line in head.decode().splitlines():
   if not line.strip():continue
   path=shlex.split(line)[0]
   if path.startswith(('usr/resource/sonix/fonts/','usr/resource/sonix/language/','etc/boot-logo/')):continue
   lines[path]=line
  overlay=ROOT/f'sonix-packer/assets/{args.model}'
  replacements={'usr/bin/sonix_player':player}
  for relative in ['usr/resource/sonix/fonts','usr/resource/sonix/language','usr/resource/sonix/components/licenses','etc/boot-logo']:
   for file in (overlay/relative).rglob('*'):
    if file.is_file():replacements[file.relative_to(overlay).as_posix()]=file
  info=json.loads((overlay/'usr/resource/sonix/components/system-info.json').read_text());info.update(OS_version=RELEASE['version'],build_version=RELEASE['build_version'],**{'ota-repo':RELEASE['repository']})
  metadata=work/'system-info.json';metadata.write_text(json.dumps(info,indent=4)+'\n');replacements['usr/resource/sonix/components/system-info.json']=metadata
  for name,file in replacements.items():
   assert file.is_file(),file
   for parent in reversed(Path(name).parents):
    key=parent.as_posix()
    if key!='.' and key not in lines:lines[key]=f'{key} D {epoch} 755 0 0'
   mode='755' if name=='usr/bin/sonix_player' else '644'
   lines[name]=f'{pseudo_name(name)} F {epoch} {mode} 0 0 cat {shlex.quote(str(file))}'
  patched=work/'geekos.pseudo';patched.write_bytes(('\n'.join(lines[k] for k in sorted(lines))+'\n').encode()+tail)
  empty=work/'empty';empty.mkdir();image=work/'geekos.squashfs'
  run([tool('mksquashfs'),empty,image,'-pf',patched,'-noappend','-comp','lzo','-b','131072','-all-root','-no-xattrs','-all-time',str(epoch),'-mkfs-time',str(epoch),'-processors','2','-no-progress','-exit-on-error'],env=env)
  payload=image.read_bytes();assert len(payload)<RELEASE['platform_inputs'][args.model]['rootfs_limit'],'Rootfs exceeds model partition'
  ota=iso/'ota_v0'
  for old in ota.glob('*rootfs.squashfs.*'):old.chmod(0o644);old.unlink()
  whole=hashlib.md5(payload).hexdigest();previous=whole;hashes=[]
  for index,offset in enumerate(range(0,len(payload),524288)):
   chunk=payload[offset:offset+524288];(ota/f'rootfs.squashfs.{index:04d}.{previous}').write_bytes(chunk);previous=hashlib.md5(chunk).hexdigest();hashes.append(previous)
  (ota/f'ota_md5_rootfs.squashfs.{whole}').write_text('\n'.join(hashes)+'\n')
  plan=ota/'ota_update.in';text=plan.read_text();parts=text.split('img_type=rootfs\n');assert len(parts)==2
  plan.chmod(0o644);plan.write_text(parts[0]+f'img_type=rootfs\nimg_name=rootfs.squashfs\nimg_size={len(payload)}\nimg_md5={whole}\n')
  for file in iso.rglob('*'):
   if not file.is_symlink():os.utime(file,(epoch,epoch))
  stamp=datetime.datetime.fromtimestamp(epoch,datetime.timezone.utc).strftime('%Y%m%d%H%M%S')+'00'
  run([tool('xorrisofs'),'-o',output,'-J','-r','--modification-date='+stamp,'--set_all_file_dates',stamp,iso],env=env)
  report=validate_iso(output,work,args.model,player);output.with_suffix('.validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()