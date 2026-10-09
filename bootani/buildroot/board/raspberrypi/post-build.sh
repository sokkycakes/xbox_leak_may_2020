#!/bin/sh
# Raspberry Pi: the kernel's modules come prebuilt with the Foundation's
# kernel8.img (rpi-firmware's modules/*-v8+), so install them from there.
set -e
TARGET_DIR="$1"
FW=$(ls -d "$BUILD_DIR"/rpi-firmware-*/ | head -n1)
rm -rf "$TARGET_DIR"/lib/modules
mkdir -p "$TARGET_DIR"/lib/modules
for d in "$FW"modules/*-v8+; do
	cp -a "$d" "$TARGET_DIR"/lib/modules/
done

# /data: the Xbox hard disk (see xbox-session), on the card's third partition.
# By device, not LABEL=: busybox mount runs before udev and has no blkid.
mkdir -p "$TARGET_DIR"/data
sed -i '/ \/data /d' "$TARGET_DIR"/etc/fstab
echo '/dev/mmcblk0p3 /data ext4 defaults,noatime,nofail 0 2' >> "$TARGET_DIR"/etc/fstab

# The display mode everything uses: NTSC 720x480, as on the Sion.
mkdir -p "$TARGET_DIR"/etc/default
echo 'KMS_MODE=720x480' > "$TARGET_DIR"/etc/default/kms
