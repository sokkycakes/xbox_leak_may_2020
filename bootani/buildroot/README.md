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

## The dashboard after the animation

When bootani finishes, [Theseus](https://github.com/MrMilenko/Theseus) (TeamUIX's
rebuild of the original Xbox dashboard, built as its desktop engine) takes the
screen. It runs on KMSDRM too, with no X11 or Wayland:

- SDL2's KMSDRM driver owns the display and creates an OpenGL 3.3 core context
  (EGL on a GBM surface). bgfx, Theseus's renderer, draws into that context
  with its OpenGL backend, and `SDL_GL_SwapWindow` does the page flip. The
  patches in `package/theseus/` add this path; on a desktop Theseus still
  uses X11/Wayland and Vulkan.
- `/etc/init.d/S99theseus` runs `/usr/libexec/theseus-session`, which waits
  for bootani to exit (KMS allows one master), then starts
  `/opt/theseus/theseus --dashboard --no-toolbar --no-boot-anim` and restarts
  it if it exits. Its output goes to `/var/log/theseus.log`.
- Input: keyboards and game controllers (Xbox pads through `xpad`, other HID
  pads through evdev). SDL finds them through eudev, which the image runs.
- Display mode: 720x480 (NTSC) everywhere by default. The kernel console
  starts there (`video=720x480@60` on the Sion command line), and bootani and
  SDL both read `KMS_MODE=720x480` from `/etc/default/kms`
  (`BR2_PACKAGE_BOOTANI_KMS_MODE`). When the display doesn't offer that mode,
  they fall back to its preferred mode, never to whatever the firmware left
  on the CRTC (`patches/sdl2/`).
- Theseus draws at 640x480 and stretches that over the display mode
  (`THESEUS_SCENE=640x480`, set in `theseus-session`), as the Xbox drew its
  dashboard into a 640x480 back buffer and its video encoder stretched it
  across the 720x480 NTSC signal. `THESEUS_SCENE=` draws at the display mode.
- The boot stick is a test loop for machines with no network. If the stick's
  EFI partition has a `theseus/` folder (the image ships one), then at every
  boot `theseus/opt/` is copied over `/opt/theseus`, `theseus/theseus.env` is
  sourced (`THESEUS_ARGS=...`, `KMS_MODE=...`), and `theseus/logs/` gets the
  displays and modes, input devices, the dashboard's log and dmesg.
- The dashboard and its data live in `/opt/theseus` (about 190 MB, in the
  initramfs like everything else). Extra arguments go in
  `/etc/default/theseus` as `THESEUS_ARGS="..."`.

Set `BR2_PACKAGE_THESEUS=n` to boot to the animation alone.

## Remote access and updates

`package/sion-tools` (on in both defconfigs) makes a running machine
reachable for testing and lets it take new images safely:

- **The stick** has two partitions. `SIONBOOT` (FAT, 1 GiB) is the EFI
  system partition with `EFI/BOOT/BOOTX64.EFI` (the whole system), room for
  two more images, and the folders a PC edits: `sion/` and `theseus/`.
  `SIONDATA` (ext4) is mounted on `/data` and grows (up to 16 GiB) on
  first boot. The root filesystem still runs from RAM, so `/data` is the
  only thing that persists.
- **Network.** `sion/sion.conf` on the stick holds the Wi-Fi network
  (`WIFI_SSID`, `WIFI_PASSWORD`), the root password and the hostname. Wired
  Ethernet and USB Ethernet adapters (ASIX, Realtek, CDC ECM/NCM, RNDIS)
  need no settings. `sion-netd` brings up each interface as it appears and
  writes its addresses to `sion/logs/network.txt` on the stick. Avahi
  announces `<hostname>.local` (default `sion.local`).
- **SSH.** OpenSSH with SFTP, root only, by password (`PASSWORD` in
  `sion.conf`) or by key (`sion/authorized_keys` on the stick, or
  `/data/sion/ssh/authorized_keys`). Host keys are kept on `/data`, so the
  machine's identity survives reboots.
- **Pushing files.** Anything copied in with `scp` lasts until reboot.
  `sion persist PATH...` copies files into `/data/overlay`, which is copied
  over `/` at every boot (before everything but the boot animation).
  `sion restart theseus` restarts the dashboard.
- **Image updates.** `sion update FILE` writes a new `BOOTX64.EFI` to
  `EFI/sion/trial.efi`, adds a UEFI boot entry for it and sets `BootNext`,
  so the firmware boots it exactly once. If that boot has `sshd` (and the
  dashboard, when the image has one) up for 30 seconds, the trial becomes
  `EFI/BOOT/BOOTX64.EFI` and the old image is kept as
  `EFI/sion/previous.efi`. If it panics (the kernel reboots after 10 s),
  hangs (the chipset watchdog resets it), or isn't healthy within 5 minutes,
  the next boot is the old image again, and the trial is kept as
  `EFI/sion/failed.efi`. `sion rollback` swaps back by hand; `--direct`
  replaces the image without a trial boot, for firmware that can't do
  `BootNext`. `sion/logs/update.txt` on the stick records each step.
- **Rebooting.** `sion reboot` boots this stick again even when the
  firmware's boot order starts another OS first: it adds a `Sion` boot
  entry for the stick (once) and points `BootNext` at it.

From a PC on the same network (Windows has `ssh` and `scp` built in):

    ssh root@sion.local sion status
    scp BOOTX64.EFI root@sion.local:/data/
    ssh root@sion.local sion update /data/BOOTX64.EFI

## Build

    git clone https://gitlab.com/buildroot.org/buildroot.git
    cd buildroot
    make BR2_EXTERNAL=/path/to/bootani/buildroot sion_bootani_defconfig
    make

Outputs in `output/images/`:

- `bzImage`: the kernel with the root filesystem built in, bootable from UEFI
- `bootani-usb.img`: a GPT disk image with that kernel as `EFI/BOOT/BOOTX64.EFI`

Write the image to a USB stick with `dd if=output/images/bootani-usb.img of=/dev/sdX bs=4M`.

Built and boot-tested (UEFI, in QEMU) with Buildroot master from 2026-09-29. A
first build compiles LLVM twice (host and target) and needs about 25 GB of disk.
bootani's messages go to `/var/log/bootani.log` on the target.

## Configurations

| Defconfig | Machine | GPU driver |
| --- | --- | --- |
| `sion_bootani_defconfig` | Acer Chromebox CXI3 ("sion", Kaby Lake) | i915 + Mesa iris (softpipe as a slow fallback) |
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
