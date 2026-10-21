#!/bin/sh
# Builds drmkeep.so (64-bit, for Conty's gamescope; needs libdrm headers).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
gcc -O2 -shared -fPIC -Wall -o "$HERE/drmkeep.so" "$HERE/drmkeep.c" -I/usr/include/libdrm -ldl
strip "$HERE/drmkeep.so"
md5sum "$HERE/drmkeep.so"
