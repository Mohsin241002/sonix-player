# Native UI preview and interaction checks

This macOS harness renders the actual Sonix LVGL home, Apps drawer, music menu,
and Appearance pages at device resolution. Audio, radios, library data and
hardware services are deterministic fixtures in `preview_services.c`. It is
not a firmware build, device screenshot, playback test or device benchmark.

Requirements: Python 3, clang, pkg-config, SDL2, FreeType and the application's
pinned LVGL 9.6.0 checkout in `sonix-player/lvgl`. Generated icons and the Inter
fonts are checked in; running this harness does not require Cairo or Pillow.

From the repository root:

```sh
python3 sonix-player/tools/native-preview/build.py
python3 sonix-player/tools/native-preview/render.py
python3 sonix-player/tools/native-preview/render.py --page home apps --height 720 800 --text normal large --language English Deutsch Chinese
python3 sonix-player/tools/native-preview/render.py --page home --height 720 800 --playing --input-test
```

Screenshots appear in the ignored `screenshots/` directory. `--playing` uses
clearly labelled demo metadata, with no audio output. `--input-test` drives
LVGL's pointer device through the Apps shortcut, all nine drawer destinations,
Back navigation, playback-button dispatch and runtime text-size switching.
Those checks test navigation and event routing, not the fixture services.

The desktop build deliberately stays outside the normal firmware Makefile;
none of these service stubs enter the application or a device build.
