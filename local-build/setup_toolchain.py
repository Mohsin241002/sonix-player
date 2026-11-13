#!/usr/bin/env python3
"""Prepare a macOS cross-build of sonix_player for the HiBy R1 (MIPS32r2, glibc 2.22).

Creates ./toolchain next to this script: Zig 0.13.0 compiler wrappers named like
the Rockbox toolchain, ALSA 1.2.13 headers, and libasound.so pulled from the
connected player over ADB (link-time only). Then writes ./build-target.sh.
Needs: curl, adb on PATH (or ADB=...), Homebrew llvm (llvm-strip, llvm-readelf).
"""
import os, platform, shutil, subprocess, tarfile
from pathlib import Path
HERE = Path(__file__).resolve().parent
APP = HERE.parent / "sonix-player"
TC = HERE / "toolchain"
ADB = os.environ.get("ADB", "adb")
LLVM = Path(os.environ.get("LLVM_BIN", "/opt/homebrew/opt/llvm/bin"))
arch = "aarch64" if platform.machine() in ("arm64", "aarch64") else "x86_64"
ZIG = f"zig-macos-{arch}-0.13.0"

def fetch(url, dest):
    if not dest.exists():
        subprocess.run(["curl", "-fL", "--retry", "3", "-o", str(dest), url], check=True)

for d in ("bin", "lib", "include/alsa", "downloads"):
    (TC / d).mkdir(parents=True, exist_ok=True)
fetch(f"https://ziglang.org/download/0.13.0/{ZIG}.tar.xz", TC / "downloads/zig.tar.xz")
if not (TC / ZIG).exists():
    with tarfile.open(TC / "downloads/zig.tar.xz") as t:
        t.extractall(TC, filter="data")
zig = TC / ZIG / "zig"
def wrapper(name, body):
    p = TC / "bin" / f"mipsel-rockbox-linux-gnu-{name}"
    p.write_text("#!/bin/sh\n" + body + "\n"); p.chmod(0o755)
for name, mode in (("gcc", "cc"), ("g++", "c++")):
    wrapper(name, f'export ZIG_GLOBAL_CACHE_DIR="{TC}/zig-cache"\nexec "{zig}" {mode} -target mipsel-linux-gnueabihf.2.22 -mcpu=mips32r2 "$@"')
for name in ("ar", "ranlib"):
    wrapper(name, f'exec "{zig}" {name} "$@"')
for name in ("strip", "readelf"):
    wrapper(name, f'exec "{LLVM}/llvm-{name}" "$@"')
w = TC / "bin/wget"  # the Makefile fetches library sources with wget -O
w.write_text('#!/bin/sh\n[ "$1" = "-O" ] || exit 2\nexec curl -fL --connect-timeout 20 --max-time 300 -o "$2" "$3"\n'); w.chmod(0o755)

fetch("https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.13.tar.bz2", TC / "downloads/alsa-lib.tar.bz2")
if not (TC / "alsa-lib-1.2.13").exists():
    with tarfile.open(TC / "downloads/alsa-lib.tar.bz2") as t:
        t.extractall(TC, filter="data")
inc = TC / "include/alsa"
for h in (TC / "alsa-lib-1.2.13/include").glob("*.h"):
    shutil.copyfile(h, inc / h.name)
names = "asoundef version global input output error conf pcm rawmidi ump timer hwdep control mixer seq_event seq seqmid seq_midi_event".split()
(inc / "asoundlib.h").write_text((inc / "asoundlib-head.h").read_text() + "".join(f"#include <alsa/{n}.h>\n" for n in names) + (inc / "asoundlib-tail.h").read_text())
(inc / "version.h").write_text('#define SND_LIB_MAJOR 1\n#define SND_LIB_MINOR 2\n#define SND_LIB_SUBMINOR 13\n#define SND_LIB_EXTRAVER 1000000\n#define SND_LIB_VER(a,b,c) (((a)<<16)|((b)<<8)|(c))\n#define SND_LIB_VERSION SND_LIB_VER(1,2,13)\n#define SND_LIB_VERSION_STR "1.2.13"\n')
if not (TC / "lib/libasound.so").exists():
    subprocess.run([ADB, "pull", "/usr/lib/libasound.so.2.0.0", str(TC / "lib/libasound.so")], check=True)

(HERE / "build-target.sh").write_text(f'''#!/bin/sh
set -eu
cd "{APP}"
export PATH="{TC}/bin:$PATH"
exec make -j6 sonix_player ROCKBOX_TOOLCHAIN_DIR="{TC}" LIB_CFLAGS='-O2 -march=mips32r2' TARGET_CFLAGS='-O2 -g -Wall -MMD -MP -D_FILE_OFFSET_BITS=64 -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE=1 -DGB_CORE=1 -march=mips32r2 -mno-madd4 -ffunction-sections -fdata-sections -Ilvgl/src -Ifreetype-target/include/freetype2 -Iaudio-target/include -Iaudio-target/include/opus -I{TC}/include -include src/system/core/glibc_compat.h' TARGET_LDFLAGS='freetype-target/lib/libfreetype.a audio-target/lib/libopusfile.a audio-target/lib/libopus.a audio-target/lib/libogg.a audio-target/lib/libwavpack.a -L{TC}/lib -lasound -lpthread -lm -ldl -Wl,--gc-sections' "$@"
''')
print("Toolchain ready. Build with: sh", HERE / "build-target.sh")
