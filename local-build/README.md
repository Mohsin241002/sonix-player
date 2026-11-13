# Local build, trial and flash for the HiBy R1 (macOS)

Personal developer tooling for this fork. It is not part of the player.

1. **Toolchain** – `python3 local-build/setup_toolchain.py` (needs the R1 on ADB once,
   Homebrew `llvm`, and the pinned LVGL 9.6.0 checkout in `sonix-player/lvgl`).
2. **Build** – `sh local-build/build-target.sh` produces `sonix-player/sonix_player`
   and runs the glibc 2.22 symbol check.
3. **Firmware image** – download the official Sonix 1.3.0 `r1.upt` (URL and SHA-256 are
   in `firmware.json`), `brew install squashfs xorriso`, bump the version fields in
   `firmware.json`, then
   `python3 local-build/build_firmware.py --platform-input r1.upt --output out.upt`.
   Only the player, fonts, languages, licences and boot logos are replaced; the
   kernel is checked to be unchanged and the root filesystem to fit its partition.
4. **Trial before flashing** – push `sonix_player`, the R1 `fonts/` and `language/`
   folders and `try.sh` to `/mnt/sd_0/.sonix-trial/`, then `adb shell sh
   /mnt/sd_0/.sonix-trial/try.sh`. It bind-mounts the candidate over the installed
   player and restores the installed one when the candidate exits; a reboot also
   restores it. `ui.c` (build static for `mipsel-linux-musl`) injects taps, swipes
   and keys and grabs the real front buffer: `ui grab <file>` writes 480x800 RGB565.
5. **Flash** – push the image as `/mnt/sd_0/r1-candidate.upt`, then
   `adb shell EXPECTED_MD5=<md5> sh install.sh`. It checks the model, battery and
   checksum, stops the player, arms recovery and reboots. Keep the previous image
   on the card (for example in `.sonix-builds/`) to roll back the same way.

This image is for the **HiBy R1 only**.
