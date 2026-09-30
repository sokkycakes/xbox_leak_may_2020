#!/bin/sh
# Keep only the GPU firmware this machine can load, so the initramfs stays small.
set -e
TARGET_DIR="$1"
FW="$TARGET_DIR/lib/firmware/i915"
if [ -d "$FW" ]; then
	find "$FW" -type f ! -name 'kbl_*' -delete
	find "$FW" -type l ! -name 'kbl_*' -delete
fi
