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
genimage --rootpath "$TARGET_DIR" --tmppath "$GENIMAGE_TMP" \
	--inputpath "$BINARIES_DIR" --outputpath "$BINARIES_DIR" \
	--config "$BOARD_DIR/genimage.cfg"
