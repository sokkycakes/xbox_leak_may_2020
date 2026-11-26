#!/bin/sh
set -e
BOARD_DIR="$(dirname "$0")"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
rm -rf "$GENIMAGE_TMP"
cp "$BOARD_DIR/theseus-README.txt" "$BINARIES_DIR/"
# The stick's sion/ folder (package/sion-tools).
SION="$TARGET_DIR/usr/share/sion"
if [ -d "$SION" ]; then
	cp "$SION/README.txt" "$BINARIES_DIR/sion-README.txt"
	sed 's/$/\r/' "$SION/sion.conf" > "$BINARIES_DIR/sion.conf"
else
	: > "$BINARIES_DIR/sion-README.txt"
	: > "$BINARIES_DIR/sion.conf"
fi
: > "$BINARIES_DIR/authorized_keys"
CONFIG=$BOARD_DIR/genimage.cfg
# A split system (package/sion-tools): BOOTX64.EFI carries only the early
# root filesystem, and the system, rootfs.squashfs, goes on SIONDATA as
# sion/root/ID.sqfs. crimson-root-ID.sqfs is what "sion update" takes with
# the BOOTX64.EFI.
ID=$(cat "$TARGET_DIR/etc/sion-root-id" 2>/dev/null || true)
if [ -n "$ID" ] && [ -e "$BINARIES_DIR/rootfs.squashfs" ]; then
	SYSTEM=$BINARIES_DIR/crimson-root-$ID.sqfs
	find "$BINARIES_DIR" -maxdepth 1 -name 'crimson-root-*.sqfs' ! -name "${SYSTEM##*/}" -delete
	cp "$BINARIES_DIR/rootfs.squashfs" "$SYSTEM"
	DATA_ROOT=$BUILD_DIR/sion-data-root
	rm -rf "$DATA_ROOT"
	mkdir -p "$DATA_ROOT/sion/root" "$DATA_ROOT/overlay"
	ln "$SYSTEM" "$DATA_ROOT/sion/root/$ID.sqfs"
	# Room for the system and 64 MiB more (it grows on first boot).
	mib=$(( $(stat -c %s "$SYSTEM") / 1048576 + 96 ))
	CONFIG=$BUILD_DIR/genimage-sion.cfg
	sed -e "s|empty = true|srcpath = \"$DATA_ROOT\"|" -e "s|size = 64M|size = ${mib}M|" \
		"$BOARD_DIR/genimage.cfg" > "$CONFIG"
fi
genimage --rootpath "$TARGET_DIR" --tmppath "$GENIMAGE_TMP" \
	--inputpath "$BINARIES_DIR" --outputpath "$BINARIES_DIR" \
	--config "$CONFIG"
