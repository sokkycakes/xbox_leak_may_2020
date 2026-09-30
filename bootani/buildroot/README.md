# bootani on Buildroot

A Buildroot external tree that builds a small Linux system whose first job
after the kernel boots is to play the Xbox boot animation, with sound, full
screen, with no X11 or Wayland.

- `bootani --kms` draws with OpenGL 3.3 core through EGL on a GBM surface
  and shows frames with KMS page flips (`src/platform/platform_kms.cpp`).
- Sound goes straight to ALSA (`src/platform/platform_alsa.cpp`).
- `/etc/init.d/S00bootani` starts it in the background as the first init
  script. On SIGTERM it hands the screen back to the console.
- The kernel is a UEFI executable with the whole root filesystem inside it
  as an initramfs, so the GPU driver, its firmware and bootani are all there
  before any disk is touched.

## Build

    git clone https://gitlab.com/buildroot.org/buildroot.git
    cd buildroot
    make BR2_EXTERNAL=/path/to/bootani/buildroot sion_bootani_defconfig
    make

Outputs in `output/images/`:

- `bzImage`: the kernel with the root filesystem built in, bootable from UEFI
- `bootani-usb.img`: a GPT disk image with that kernel as `EFI/BOOT/BOOTX64.EFI`

Write the image to a USB stick with `dd if=output/images/bootani-usb.img of=/dev/sdX bs=4M`.

Tested against Buildroot master from 2026-09-29.

## Configurations

| Defconfig | Machine | GPU driver |
| --- | --- | --- |
| `sion_bootani_defconfig` | Acer Chromebox CXI3 ("sion", Kaby Lake) | i915 + Mesa iris |
| `qemu_x86_64_bootani_defconfig` | QEMU, for testing | virtio-gpu + Mesa softpipe |

The Sion build needs LLVM for Mesa's iris driver, so the first build takes a
while. Any other Intel GPU from Broadwell on works with the same config;
change `board/sion/linux.fragment` for other hardware.

To try the QEMU image:

    qemu-system-x86_64 -m 1024 -smp 2 -kernel output/images/bzImage \
        -device virtio-vga -device intel-hda -device hda-duplex \
        -serial stdio

softpipe renders on the CPU, so it plays much slower than real time in
QEMU, but it runs the same code path as real hardware.

## Booting the Chromebox

A Chromebox only boots its own OS until its firmware is changed. With
[MrChromebox's firmware](https://docs.mrchromebox.tech/) installed
(the UEFI Full ROM, or RW_LEGACY in developer mode), plug in the USB stick
and pick it from the boot menu. To make it the machine's only OS, copy
`bzImage` to the internal disk's EFI system partition as
`EFI/BOOT/BOOTX64.EFI`.

## Options

`BR2_PACKAGE_BOOTANI_ARGS` (in `make menuconfig`, under External options)
sets bootani's command line at boot. It defaults to `--kms`. You can add:

- `--hold` keeps the final logo on screen until the init script is stopped
  (for example, by whatever starts next).
- `--short` plays the warm-boot variant.
- `--audio-device NAME` picks an ALSA device.
- `--drm-device /dev/dri/cardN` picks a GPU.

The file is written to `/etc/default/bootani` on the target.

## Sound on the Chromebox

The image plays through the HDMI/DisplayPort output using the legacy HD Audio
driver (`snd_intel_dspcfg.dsp_driver=1` on the kernel command line). bootani
tries the ALSA default device first, then `hdmi:CARD=PCH,DEV=0`. The
headphone jack is on an I2S codec behind the audio DSP, which this image does
not include. `aplay -l` on the target lists what is there.
