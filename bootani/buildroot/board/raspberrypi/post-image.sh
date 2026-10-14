#!/bin/bash
# Raspberry Pi SD card: the firmware, the Foundation's kernel8.img and the
# device trees on the boot partition, this image's root filesystem, and an
# empty /data partition.
set -e
FW=$(ls -d "$BUILD_DIR"/rpi-firmware-*/ | head -n1)
cp "$FW"boot/kernel8.img "$BINARIES_DIR"/rpi-firmware/kernel8.img

FILES=()
for i in "$BINARIES_DIR"/rpi-firmware/*; do
	FILES+=( "${i#${BINARIES_DIR}/}" )
done
BOOT_FILES=$(printf '\\t\\t\\t"%s",\\n' "${FILES[@]}")
sed "s|#BOOT_FILES#|${BOOT_FILES}|" "$(dirname "$0")/genimage.cfg.in" > "$BINARIES_DIR"/genimage.cfg

trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"
rm -rf "$BUILD_DIR"/genimage.tmp
genimage --rootpath "$ROOTPATH_TMP" --tmppath "$BUILD_DIR"/genimage.tmp \
	--inputpath "$BINARIES_DIR" --outputpath "$BINARIES_DIR" --config "$BINARIES_DIR"/genimage.cfg
