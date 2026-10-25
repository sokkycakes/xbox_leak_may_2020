# Qt Extended (Qtopia) on the Anbernic RG DS

Work in progress. sokky's Qtopia 4.3.2 Greenphone SDK holds only prebuilt
(old-ABI ARM) binaries, so this rebuilds from Qt Extended 4.4.3, the release
after 4.3.2, as carried by QtMoko (github.com/radekp/qtmoko, master
cdaa420a, Qt submodule v4.8.3-qtmoko-v52).

- `device/rgds/`: QtMoko device profile for the RG DS (aarch64 cross mkspec,
  640x480, no modem) and a KMS/DRM QWS screen driver (`kms`), so no fbdev,
  X11 or Wayland is needed.
- `patches/`: changes to QtMoko's Qt 4.8 (AArch64 atomics, C++98 mode for
  modern GCC, fixes).
- `buildroot/`: BR2_EXTERNAL tree; `qemu_aarch64_qtopia_defconfig` is the
  QEMU stand-in for the RG DS image.

## Status (2026-10-17)

Runs in QEMU (aarch64, Linux 6.18.7, KMS only): first-use setup, home screen
and app grid on the top panel, Qt Extended's secondary clock display on the
bottom panel, touch on both panels. Not done yet: RG DS board config (RK3568
DTS, real input device names), pad button mapping, SSL (needs an OpenSSL 3
port), media playback (QtMoko's engine wants gstreamer 0.10), camera.

## Building (cloud side)

1. Buildroot 2026.08 with `BR2_EXTERNAL=qtopia-rgds/buildroot`,
   `make qemu_aarch64_qtopia_defconfig && make` (gives the toolchain and sysroot).
2. `sudo ./setup-sdk.sh <buildroot output dir>` links them under `/opt/rgds-sdk`.
3. QtMoko at cdaa420a: copy `device/rgds` to `devices/rgds`, apply
   `patches/qtmoko-build-fixes.patch` to QtMoko and
   `patches/qt-4.8.3-qtmoko-v52-modern-gcc-aarch64.patch` to `qtopiacore/qt`.
4. In a build dir: `<qtmoko>/configure -device rgds -build-qt -j 4`,
   `bin/qbuild -j8`, then
   `LD_LIBRARY_PATH=$PWD/sdk/qtopiacore/host/lib bin/qbuild image`.
5. Re-run Buildroot's `make` with `QTEXTENDED_IMAGE=<build dir>/image`; the
   post-build script puts it in `/opt/qtmoko`.

`qemu/run-qemu.sh` boots the result (each QEMU window is one panel).

## Display and input settings

`/etc/init.d/S90qtextended` starts `qpe` and restarts it if it exits (log in
`/tmp/qtextended/qpe.log`). `/etc/qtextended.conf`, when present, overrides:

- `QWS_DISPLAY="Multi: kms:connector=0:0 kms:connector=1:offset=0,480:1 :0"`:
  `kms` options are `dev=` (default `/dev/dri/card0`), `connector=N` (Nth
  connected output), `mmWidth=`/`mmHeight=`. `KMS_DEBUG=1` logs flushes.
- `QWS_MOUSE_PROTO="LinuxInput:/dev/input/eventA:screen=0 LinuxInput:/dev/input/eventB:screen=1"`:
  one touch device per panel. The event numbers differ per board.
