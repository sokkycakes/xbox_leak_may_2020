#!/bin/bash
#
# Boot the Raspberry Pi image in QEMU's Raspberry Pi 3B model, for testing
# without a Pi. QEMU doesn't model the VideoCore GPU, so there is no
# /dev/dri: xbcompat falls back to SDL's offscreen driver with Mesa's
# softpipe and writes screenshots instead of drawing on HDMI. Its speed says
# nothing about a real Pi's.
#
# usage: qemu-run.sh [IMAGES_DIR]   (default: output/images)
# Serial console (root / xbox) on stdio; Ctrl-A X quits.
#
set -e
IMAGES=${1:-output/images}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Bluetooth takes the PL011 UART; disable-bt gives it back to the console.
fdtoverlay -i "$IMAGES/rpi-firmware/bcm2710-rpi-3-b.dtb" \
	-o "$WORK/rpi3.dtb" "$IMAGES/rpi-firmware/overlays/disable-bt.dtbo"
# QEMU wants the SD card size to be a power of two.
cp "$IMAGES/sdcard.img" "$WORK/sd.img"
truncate -s 2G "$WORK/sd.img"

qemu-system-aarch64 -M raspi3b -kernel "$IMAGES/rpi-firmware/kernel8.img" -dtb "$WORK/rpi3.dtb" \
	-drive file="$WORK/sd.img",if=sd,format=raw \
	-append "root=/dev/mmcblk0p2 rootwait console=ttyAMA0,115200 video=HDMI-A-1:720x480@60" \
	-serial mon:stdio -display none -usb -device usb-kbd
