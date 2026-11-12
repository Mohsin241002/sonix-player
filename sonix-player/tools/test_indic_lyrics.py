#!/usr/bin/env python3
"""Render Indian-script lyric samples using the real LVGL/FreeType adapter.

Needs a C/C++ compiler, pkg-config, FreeType and HarfBuzz development packages.
Builds a display-free LVGL copy in the build cache; never opens SDL or audio.
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = (args.output or root / 'build_gen' / 'indic-tests').resolve()
out.mkdir(parents=True, exist_ok=True)
config = re.sub(r'(#define LV_USE_(?:SDL|LINUX_FBDEV|EVDEV)) [01]', r'\1 0', (root / 'lv_conf.h').read_text())
config_path = out / 'lv_conf.h'
if not config_path.exists() or config_path.read_text() != config:
    config_path.write_text(config)
cc = shlex.split(os.environ.get('CC', 'cc'))
cxx = shlex.split(os.environ.get('CXX', 'c++'))
flags = ['-g', '-O1', '-fno-omit-frame-pointer', '-Wno-cpp',
         f'-DLV_CONF_PATH="{config_path}"', '-DHOST_BUILD=1', '-I'+str(root), '-I'+str(root/'lvgl')]
if args.sanitize:
    flags += ['-fsanitize=address,undefined']
flags += shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'freetype2', 'harfbuzz'], text=True))
sources = list((root/'lvgl/src').rglob('*.c')) + [root/'src/gui/fonts/indic.c', root/'tools/test_indic_lyrics.c']

def compile_one(source):
    obj = out/'obj'/source.relative_to(root).with_suffix('.o')
    obj.parent.mkdir(parents=True, exist_ok=True)
    # Keep each build's exact command alongside the object, including sanitizer
    # flags and configuration, so changing modes cannot reuse incompatible files.
    command = cc + flags + ['-c', str(source), '-o', str(obj)]
    stamp = obj.with_suffix('.command')
    signature = repr(command)
    headers = list((root/'src/gui/fonts').glob('*.h'))
    newest = max(p.stat().st_mtime for p in [source, config_path, *headers])
    if obj.exists() and stamp.exists() and stamp.read_text() == signature and obj.stat().st_mtime >= newest:
        return obj
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(str(source) + '\n' + result.stderr)
    stamp.write_text(signature)
    return obj

with concurrent.futures.ThreadPoolExecutor(max_workers=min(os.cpu_count() or 2, 8)) as pool:
    objects = list(pool.map(compile_one, sources))
link = cxx + (['-fsanitize=address,undefined'] if args.sanitize else [])
link += ['-o', str(out/'test-indic')] + [str(p) for p in objects]
link += shlex.split(subprocess.check_output(['pkg-config', '--libs', 'freetype2', 'harfbuzz'], text=True)) + ['-lpthread', '-lm']
subprocess.run(link, check=True)
env = os.environ.copy()
env.setdefault('SONIX_INDIC_FONT_DIR', str(root.parent/'sonix-packer/assets/R1/usr/resource/sonix/fonts/indic'))
subprocess.run([str(out/'test-indic'), str(out/'preview.ppm')], env=env, check=True)
