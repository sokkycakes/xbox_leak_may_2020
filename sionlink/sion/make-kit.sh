#!/bin/sh
# Builds Sion Link and assembles the push kit in $1.
#   needs: BlockSDS (/opt/wonderful), a static hostapd at $HOSTAPD
set -e
OUT=${1:?usage: make-kit.sh OUTDIR}
HOSTAPD=${HOSTAPD:?set HOSTAPD to a static hostapd binary}
here=$(cd "$(dirname "$0")" && pwd)
make -C "$here/../host"
PATH=/opt/wonderful/bin:$PATH make -C "$here/../ds"
mkdir -p "$OUT/files"
cp "$here/../ds/sionlink.nds" "$OUT/"
cp "$here/../host/sionlinkd" "$HOSTAPD" "$here/sion-dslink" \
   "$here/hostapd-sionlink.conf.in" "$here/install-sionlink.sh" "$OUT/files/"
strip "$OUT/files/hostapd" "$OUT/files/sionlinkd"
cp "$here/sion-dslink.sh" "$here/sion-dslink.ps1" "$here/Sion DS link.cmd" "$OUT/"
chmod 755 "$OUT/sion-dslink.sh" "$OUT/files/"*
