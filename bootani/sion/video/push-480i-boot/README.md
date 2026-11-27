# push-480i-boot: the whole boot at 15 kHz, without a new image

push-480i gets the dashboard, games and Guide to 2880x480i. The image under
it still has `video=720x480@60` in its built-in kernel command line, so the
kernel's text console (boot messages, and the tty whenever nothing holds the
screen, such as between programs or on a VT switch) sends 720x480p, 31 kHz,
and the boot animation starts in 480p before the dashboard's sion-video runs.

This kit fixes that from the boot entry, for the image the Sion runs now
(xbox-consolidated d848fec0, BOOTX64.EFI md5 f6f787b2...):

- **`video=HDMI-A-1:2880x480M@30i`**: added to the "Sion" boot entry, which
  the kernel reads after its built-in command line; a video= naming the
  connector beats the global one. The console gets a 2880x480 interlaced
  framebuffer from the kernel's first frame, at the kernel's CVT timing:
  15.11 kHz lines, 59 fields (3600x512 at 54.39 MHz). Exact NTSC (VIC 10)
  can't be had this way: the kernel's GTF interlaced modes lose the
  interlace flag (2880x240 progressive at 31 kHz), and CVT at `@60i` makes
  60 frames (31 kHz, 120 fields).
- **`initrd=\sion\video-boot.cpio`**: on the EFI partition, unpacked over the
  image's files at boot: the new sion-video and its EDID, S00bootani running
  `sion-video early` (the EDID goes on and KMS_MODE becomes 2880x480i before
  the boot animation), and bootani patched to fill a 2880x480i screen (one
  jump in its 4:3 check turned into a no-op, as in platform_kms.cpp).

So the firmware's splash is the only thing left at the adapter's own mode;
that comes before Linux and no software here can change it.

The dashboard, games and Guide keep the exact VIC 10 timing (15.734 kHz,
59.94 fields); the console stays on the CVT one, so a switch between them is
a mode change, at 15 kHz both ways.

## Install

As root on the Sion, from the kit folder: `sh sion-install.sh`. It checks
MD5SUMS, copies video-boot.cpio to the EFI partition (sion/), replaces this
partition's "Sion" boot entry with one carrying the options (first in the
boot order), and puts the new sion-video in /data/overlay. Nothing restarts;
it applies from the next boot. If the firmware can't start the entry, it
falls back to its own SATA entry, which boots the image without the options.

`sh sion-install.sh --remove` puts the plain entry back and deletes the cpio.
Run it when an image with the 15 kHz kernel (drm.edid_firmware in
board/sion/linux.fragment) goes in: that image doesn't need the options, and
the cpio's files would sit over its own.

## Tested

In the cloud: the Sion's own image (f6f787b2) under OVMF on a SIONBOOT disk:
the installer made the entry, the next boot came up through it with the cpio
applied (S00bootani ran sion-video early, patched bootani in place) and went
on to the dashboard. The console timing was checked on the 6.18.7 kernel in
QEMU (std VGA, interlace allowed as on i915): `2880x480M@30i` gives
`54391 2880 2960 3240 3600 480 483 493 512` interlaced.
