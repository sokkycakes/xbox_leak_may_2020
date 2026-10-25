# Mirror test results (step 1, cloud, 2026-10-22)

All 12 demos replay correctly. Two melonDS instances were used: one ran the demo and recorded a
bundle per frame, the other ran our DS replay client, which redraws each frame on the emulated
DS's own 2D/3D hardware. Each demo ran 420 frames (7 s) with a scripted button sequence.
Code: branch `claude/project-thread-lkot3d`, `sionlink/mirror/`.

| Demo | Frames identical | Bundle avg (raw / zlib) |
|---|---|---|
| graphics_3d_2d_and_3d (2D bg + 3D + sprite) | 420/420 | 0.4 / 0.2 KB |
| graphics_3d_basic_texture | 419/420 | 0.3 / 0.2 KB |
| gl2d_2d_and_3d | 419/420 | 1.0 / 0.4 KB |
| graphics_2d_bg_scroll_bigmap | 418/420 | 0.4 / 0.1 KB (45 KB peak on scrolls) |
| graphics_2d_sprites_animated | 419/420 | 0.1 / 0.03 KB |
| video_effects_blending | 419/420 | 0.2 / 0.1 KB |
| video_effects_hblank_scroll_dma (per-line wave) | 419/420 | 1.5 / 0.8 KB |
| video_capture_dual_screen_3d (3D on both screens) | 419/420 | 0.8 / 0.3 KB |
| video_capture_motion_blur | 419/420 | 1.3 / 0.5 KB |
| graphics_3d_fog (dense model) | 418/420 | 41.7 / 5.0 KB |
| graphics_3d_toon_shading (teapot) | 419/420 | 43.7 / 16.8 KB |
| graphics_3d_volumetric_shadow | 418/419 | 44.7 / 17.1 KB |

- The frames that differ are the first one or two after boot, where the demo switches the
  screens on partway through a frame, before the recorder has a start-of-frame snapshot.
  Nothing differs after that.
- Bundles are tiny for 2D and light 3D. Dense 3D models cost about 42-45 KB a frame raw and
  5-17 KB with zlib, because the whole model is re-sent every frame.
- Fixed along the way: per-line register effects are now replayed from an HBlank IRQ;
  display captures are redone by the client's own hardware instead of being sent as pixels
  (dual_screen_3d went from 43 KB to 0.8 KB a frame); POWCNT is sent in order with the 3D
  writes.
- Not tested yet: real hardware and a live link.

## Mario Kart DS kiosk demo (2026-10-22)
The uploaded kiosk demo (A39E, an E3 2005 build) boots with `--gamecode AMKE`. melonDS's cart
list gives A39E to a NAND-save cart, so every ROM read returned 0xFF and the game crashed
looking for its sound archive. Input script `harness/inputs/mkds.txt`: menus with defaults, then
Figure-Eight Circuit with A held. 3000 frames.

- 2636 of 2992 frames are pixel-identical (88%). All mismatches are frames 907-1263, the cup
  select screen: the course thumbnail is garbled and the bottom screen is blank on the client
  (`mkds/menu-mismatch.png`). Fixed later, see below.
- The whole race (frames 1264-3000) matches on both screens (`mkds/race-frame2252.png`). Only
  the course flyover before the race (frames 1337-1460) draws 3D on both screens through display
  capture; in the race itself the bottom map is 2D.
- Size, race: 84 KB/frame raw (almost all 3D commands), 134 KB p95, 38 KB with zlib. That is
  about 5 MB/s raw and 2.2 MB/s zlib at 60 fps, over the DSpico's ~1.2 MB/s.
- Menus: 14 KB/frame raw on average, 302 KB peak (course loading VRAM uploads), 5 KB zlib.
- 97% of each race frame's 3D command bytes (64-byte chunks) already appeared in the previous
  frame, so sending only changed command blocks should bring the race down to a few KB a frame
  (estimate, not built).
- `mkds/sizes.csv` and `mkds/diff.csv` have per-frame numbers.

### 30 fps mode (`--half 1`)
The recorder sends every other frame and the client shows each for two VBlanks. A skipped frame's
3D commands are parsed: matrix, material, light, polygon/texture attribute, color and viewport
commands are kept (re-sent as direct command port writes), geometry, tests and SWAP_BUFFERS are
dropped. While the game captures (3D on both screens, one per frame) every frame is sent.
- All 1561 sent frames pixel-identical.
- Kept state is about 8% of a skipped frame's commands (897k kept, 10.5M dropped).
- Race: 2.74 MB/s raw, 1.17 MB/s zlib (60 fps: 4.92 / 2.20). Whole run: 0.71 MB/s zlib.
- `mkds/sizes-30fps.csv` has per-frame numbers.

### Repeated 3D commands sent as copies (`--gxdelta 1`)
Each bundle's 3D command payload is encoded against the previous bundle's: runs of words found in
the previous payload are sent as (offset, length) copies, the rest as literals (MREC_GXDELTA in
`common/mirror_format.h`). The client rebuilds the payload and pushes it as before.
- 60 fps: 2992 of 2992 frames pixel-identical. Race 83 KB/s raw, 39 KB/s zlib (was 4.9 MB/s /
  2.2 MB/s). Whole run 33 KB/s zlib; 99% of bundles under 2.4 KB zlib, the largest 76 KB (menu
  loads).
- 30 fps: 1561 of 1561 identical. Race 81 KB/s raw, 34 KB/s zlib. At 30 fps the state kept from
  skipped frames costs about as much as the frames saved, so 30 fps no longer saves bandwidth.
- The 60 fps cup select mismatch is fixed: the client's VBlank work for a big bundle ran past line
  20, where the game turns on BG extended palettes with a mid-frame write, and the client skipped
  line changes it had already passed. It now writes those late and finishes a frame's line
  changes before the next bundle.
- `emulator-vs-replay.png` shows four of the demos side by side. `results.log` has the raw
  output.

## Live link over a simulated DSpico (2026-10-22)
Two melonDS runs paced at the DS frame rate. The "Sion" run plays the MKDS kiosk demo with
`--serve sim:...` and gets no input script: its only pad is the DS. The "DS" run boots the
live client (no recording inside), which shows blue, comes up as a USB device through the
DSpico card commands, and is enumerated by the simulated DSpico's host (1209:0001, "Sion Link
DS", bulk OUT 01 / IN 81). The DS run's input script presses the buttons.

- Stream at 12000 B/frame (~700 KB/s, a guess for the DSpico): 1426 of 1600 frames sent, 174
  left out while big VRAM uploads (100-520 KB scene changes) were still going over. 1417 frames
  compared, all pixel-identical. One earlier run had one frame at a menu change showing part
  of the old VRAM (bundle too big for the client's VBlank); timing-dependent.
- At 4000 B/frame (~240 KB/s): 1245 sent, 355 left out, still identical apart from that frame.
- Input: ~1540 pad reports reached the Sion side and took the game from the title screen
  through the menus into the race (`live/sion-side-driven-by-ds-pad.png`: frames 600, 1000,
  1400, 1590 of the Sion side).
