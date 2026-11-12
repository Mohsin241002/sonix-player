#!/usr/bin/env python3
"""Build the Sonix native LVGL preview on macOS."""
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent
APP = ROOT.parents[1]
LVGL = APP / "lvgl"
BUILD = ROOT / "build"
SOURCES = [
    "src/gui/shell/main_menu.c", "src/gui/shell/theme.c", "src/gui/shell/gridpage.c",
    "src/gui/shell/settingsrow.c", "src/gui/shell/icons.c", "src/gui/shell/topbar.c",
    "src/gui/shell/clocklabel.c", "src/gui/shell/switcher.c", "src/gui/shell/corners.c",
    "src/gui/fonts/fonts.c", "src/gui/library/music.c", "src/gui/settings/appearance.c",
    "src/system/core/config.c", "src/system/core/lang.c",
]

def pkg_config(*args):
    return shlex.split(subprocess.check_output(["pkg-config", *args], text=True))

def compile_one(source):
    relative = source.relative_to(APP) if source.is_relative_to(APP) else Path(source.name)
    obj = BUILD / relative.with_suffix(".o")
    dep = obj.with_suffix(".d")
    if dep.exists() and obj.exists():
        dependencies = shlex.split(dep.read_text().replace(chr(92) + "\n", "").splitlines()[0].split(":", 1)[1])
        if all(Path(name).exists() and Path(name).stat().st_mtime_ns <= obj.stat().st_mtime_ns
               for name in dependencies):
            return obj
    obj.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([
        "clang", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
        "-DHOST_BUILD=1", "-DLV_CONF_INCLUDE_SIMPLE=1", "-I", str(APP), "-I", str(LVGL),
        *cflags, "-MMD", "-c", str(source), "-o", str(obj)
    ], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{source}\n{result.stderr}")
    if result.stderr and not source.is_relative_to(LVGL):
        print(result.stderr, end="")
    return obj

if not (LVGL / "lvgl.h").is_file():
    raise SystemExit("Install the application's pinned LVGL 9.6.0 checkout into " + str(LVGL))

cflags = pkg_config("--cflags", "freetype2", "sdl2")
sources = [*sorted((LVGL / "src").rglob("*.c")), *(APP / name for name in SOURCES),
           ROOT / "preview.c", ROOT / "preview_services.c"]
with ThreadPoolExecutor(max_workers=min(os.cpu_count() or 4, 12)) as pool:
    objects = list(pool.map(compile_one, sources))
link = subprocess.run([
    "clang", "-Wl,-dead_strip", *map(str, objects),
    *pkg_config("--libs", "freetype2", "sdl2"), "-o", str(BUILD / "sonix_preview")
])
raise SystemExit(link.returncode)
