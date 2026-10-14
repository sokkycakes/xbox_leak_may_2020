# Raspberry Pi 3 performance

Running record of the Pi 3 performance work: what was measured, what changed,
and what it did. Plan: https://claude.ai/code/artifact/2d5a3303-7e59-40ad-8bc0-03ed15e4ab54

Ground rules: every change is pushed before it is tested on the Pi, the Pi is
updated over the network (never reflashed: that wipes /data), and each push
backs up what it replaces.

## Baseline (2026-10-11)

Marble Blast, 720x480 composite NTSC, native renderer under box86.

| Setting | Frame time |
| --- | --- |
| ondemand governor (default), CPU held at 1.0 GHz | 20.9 ms |
| performance governor, 1.2 GHz | ~17.1 ms (16.9-17.6), 58-59 fps |

No undervoltage (hwmon in0_lcrit_alarm 0). The Foundation kernel has no
soc:firmware/get_throttled node.

Earlier profile of the main thread at 15.6 ms: translated game code 9.0,
Mesa 2.9, renderer 2.1, libc 1.6. GPU about 24 ms with 4x MSAA (scene 17,
resolve 2.4, output filter 4.7). Phantom Dust 134.8 ms a frame (77 CPU,
34.5 GPU).

## Step 1: timing and clocks

- `bootani/buildroot/board/raspberrypi/S02cpufreq` sets the performance
  governor at boot.
- The Xbox vblank follows the display (src/kernel/ke.c). It used to be a free
  60.000 Hz timer, as was BlockUntilVerticalBlank, while the KMS flip runs
  at ~59.94 Hz, so the two drifted and a title waiting for a fake blank could
  lose a whole frame at the real swap. The DPC thread now reads the last real
  blank with DRM_IOCTL_WAIT_VBLANK (relative, sequence 0: no wait), learns
  the period, and fires the guest vblank 1 ms after each real one.
  BlockUntilVerticalBlank waits on that vblank (src/hle/d3d8.c). The log
  says `vblank: following the display on /dev/dri/cardN (CRTC n, x Hz)`, or
  `vblank: no display blanks to follow, using a 60 Hz timer` without DRM
  (Xvfb, fixed step, `XBCOMPAT_VBLANK=timer`).
- `tools/pi/clocks.sh [seconds]` prints the CPU, ARM, core, V3D and SDRAM
  clocks, the undervoltage alarm and the temperature once a second.
- To try: `SDL_VIDEO_DOUBLE_BUFFER=1` in /etc/default/xbox (non-blocking
  KMSDRM swap).

Results on the Pi: pending.

## Think Tanks: what a frame is made of (2026-10-14)

Captured in xemu (the user's MCPX/BIOS/HDD, the Arcade card's Think Tanks
launched through the Arcade with its launch data), QuickPlay Battlemode on
Medulla Mesa, NV2A methods logged with xemu's `nv2a_pgraph_*` trace events:

- ~730 draws a frame, one render target (the 640x480 back buffer, no MSAA:
  the title asks for multisample type 0x11), one clear, no render-to-texture
  or post-processing passes. The radar and HUD only move the clip rect.
- 416 indexed triangle lists of ~41 indices (indices inline in the push
  buffer, vertices from arrays), ~170 indexed triangle strips (terrain,
  65-95 indices), 60 four-vertex fans (HUD and sprites).
- Between 434 of those draws the title sets nothing but the textures it
  already had: same shader, constants and state, and in xbcompat's trace
  each draw's indices start where the previous draw's end. The terrain
  strips can't join that way: the title swaps the vertex program, its
  constants and the combiners between every strip (two passes per block).
- Textures by binds a frame: DXT5 256x256 (500), A8R8G8B8 64x64 (645),
  X8R8G8B8 128x128 (478), DXT1 256 and 512 (115).

xbcompat sent the same ~850 draws to GL, and its conversion cache missed
~300 times a frame, re-uploading 7.5-12.7 MB of indices and vertices: each
small draw had its own index entry, and ~700 of them didn't fit.

Changes, measured under llvmpipe in the cloud (Pi results pending,
bootani/pi/bench request 5):

- Draw merging (b9048a43, `XBCOMPAT_MERGE_DRAWS=0` turns it off): such a
  run goes to GL as one glDrawElements. ~490 of ~850 draws a frame become
  ~28 GL draws; cache uploads fall to ~30 (0.3 MB) a frame; xbcompat's draw
  time from ~150 to ~65 ms. Merged and unmerged frames match where the
  scene doesn't move.
- 16-bit DXT (d119671c, `XBCOMPAT_DXT16`): on VC4/V3D, opaque DXT textures
  go up as RGB565 and on/off-alpha ones as RGB5_A1 instead of Mesa's 32-bit
  expansion: 54 of the match's 95 DXT textures, at most 3/255 off per
  channel. `XBCOMPAT_DXT16=2` also sends the other 41 as RGBA4444.
