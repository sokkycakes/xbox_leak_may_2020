#!/bin/sh
set -e
BOARD_DIR="$(dirname "$0")"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
rm -rf "$GENIMAGE_TMP"
cp "$BOARD_DIR/theseus-README.txt" "$BINARIES_DIR/"
genimage --rootpath "$TARGET_DIR" --tmppath "$GENIMAGE_TMP" \
	--inputpath "$BINARIES_DIR" --outputpath "$BINARIES_DIR" \
	--config "$BOARD_DIR/genimage.cfg"
