#!/bin/sh
# Builds DSWifi at BASE with sionlink.patch into OUT (default ../build/dswifi):
# OUT/include and OUT/lib/{libdswifi7.a,libdswifi9_noip.a}.
set -e
here=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$here/../build/dswifi}
SRC=$OUT/src
: "${BLOCKSDS:=/opt/wonderful/thirdparty/blocksds/core}"
: "${WONDERFUL_TOOLCHAIN:=/opt/wonderful}"
export BLOCKSDS WONDERFUL_TOOLCHAIN

if [ ! -d "$SRC/.git" ]; then
    rm -rf "$SRC"
    git clone -q https://github.com/blocksds/dswifi.git "$SRC"
fi
git -C "$SRC" checkout -q -f "$(cat "$here/BASE")"
git -C "$SRC" apply "$here/sionlink.patch"
make -C "$SRC" -f Makefile.arm9 --no-print-directory
make -C "$SRC" -f Makefile.arm7 --no-print-directory
mkdir -p "$OUT/lib"
cp "$SRC/lib/libdswifi7.a" "$SRC/lib/libdswifi9_noip.a" "$OUT/lib/"
rm -rf "$OUT/include"
cp -r "$SRC/include" "$OUT/include"
