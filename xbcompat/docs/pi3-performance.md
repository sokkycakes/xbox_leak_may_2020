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
