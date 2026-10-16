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
