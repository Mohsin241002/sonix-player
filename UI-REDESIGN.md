# Sonix native UI redesign

Working branch: `feature/apple-inspired-ui`, recovered from the published EPUB
branch at `7418c55`. The later local Sonix/Indic and playback fixes were deleted
with the former workspace and were not present in that remote branch. This
checkout must not be treated as a replacement for the last installed build.
GeekOS remains outside this work.

## Current implementation

- A home screen with the current track, artwork, progress, Previous/Play/Next,
  and direct Music, Books, Settings and Apps shortcuts.
- A separate three-column Apps drawer: Music, Books, Audiobooks, Streaming,
  Wireless, Files, DAC, More and Settings. Books and Files retain their original
  refresh-on-open actions. Normal Back navigation returns through the drawer.
- Compact destination rows inside library and utility sections, preserving
  saved library ordering and actions, plus segmented Appearance controls.
- Inter Regular/SemiBold with their OFL licence and the existing script font
  fallback chains. This does not use Apple's proprietary font assets.
- Both existing light/dark themes, accent choices, languages and text sizes.
  Sonix branding, streaming and local-network services remain available.

## Performance design

The home player receives the existing playback snapshot and does no UI work
while hidden. It starts no timer and decodes no images. Artwork is copied from
an already decoded, settled cover into one 144×144 RGB565 buffer (41,472 bytes)
when its identity changes. The buffer belongs to the home screen, so a player
track change cannot leave a dangling pointer. This is a bounded addition, not
an R1 RAM measurement.

The Apps drawer uses a 160 ms position animation, cancelled on exit, with no
blur or screen-sized opacity layer. Press feedback is a 100 ms background
transition. Existing player-sheet and Back gestures remain in use. Menu glyph
payloads are 1,652,736 bytes smaller than the recovered baseline; compiled
asset savings are not equivalent to measured resident-memory savings.

## Verification and remaining work

The reproducible native preview and pointer tests are in
`sonix-player/tools/native-preview`. They render production LVGL widgets with
fixture hardware services. Home/drawer navigation, all nine destinations,
Back, transport dispatch and dynamic text-size switching were checked.
Previews cover 480×720 and 480×800, both themes, both text sizes and
English/German/Chinese. The actual player integration was also syntax-checked.

The preview is not a complete application build. No MIPS firmware was built or
installed, and hardware RAM, frame rate, sound, Bluetooth and suspend behaviour
have not been revalidated. Recover the later EPUB/Indic/playback changes and
restore the device toolchain before preparing an installable build. Full
Now Playing, the reader and service-specific detail screens still use their
existing layouts with the shared font/theme changes.
