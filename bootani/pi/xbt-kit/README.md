# xbt on the Pi 3, no reflash

The Pi image already boots the Foundation's 64-bit kernel (`kernel8.img`,
`arm_64bit=1`, 4K pages) under its 32-bit userland. xbt here is one
static-pie AArch64 binary with no libraries, so it runs on that image as is.

From the Linux PC: `sh push-xbt.sh` (report lands in `xbt-report.txt`).
Undo: `sh push-xbt.sh undo`, which deletes `/data/xbt`. Nothing else on the
Pi changes: no root remount, `/opt/xbcompat` and `/etc` untouched, the
dashboard keeps running.

The test is headless, because xbt has no GPU, sound or input yet:
preflight, the README benchmarks (20 repetitions each) timed under xbt and
the Pi's own box86, then the recovery dashboard booted for 20 s with stats.

Rebuild with `build-kit.sh`.
