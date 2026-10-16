#!/bin/sh
# Builds Sion Link and assembles the push kit in $1.
#   needs: BlockSDS (/opt/wonderful), a static hostapd at $HOSTAPD
set -e
OUT=${1:?usage: make-kit.sh OUTDIR}
HOSTAPD=${HOSTAPD:?set HOSTAPD to a static hostapd binary}
here=$(cd "$(dirname "$0")" && pwd)
make -C "$here/../host"
make -C "$here/ratefix" KDIR=${KDIR:?set KDIR to a linux-6.18.7 tree configured like the Sion} KBUILD_MODPOST_WARN=1
PATH=/opt/wonderful/bin:$PATH make -C "$here/../ds"
PATH=/opt/wonderful/bin:$PATH make -C "$here/../demo"
PATH=/opt/wonderful/bin:$PATH make -C "$here/../linktest/ds"
make -C "$here/../linktest/sion"
mkdir -p "$OUT/files"
cp "$here/../ds/sionlink.nds" "$here/../demo/sion-hello.nds" "$OUT/"
cp "$here/../host/sionlinkd" "$HOSTAPD" "$here/sion-dslink" \
   "$here/hostapd-sionlink.conf.in" "$here/install-sionlink.sh" \
   "$here/ratefix/iwl_ratefix.ko" "$here/../linktest/sion/linktest" \
   "$here/../linktest/ds/linktest.nds" "$OUT/files/"
strip "$OUT/files/hostapd" "$OUT/files/sionlinkd" "$OUT/files/linktest"
cp "$here/sion-dslink.sh" "$here/sion-linktest.sh" "$here/sion-dslink.ps1" "$here/Sion DS link.cmd" "$OUT/"
chmod 755 "$OUT/sion-dslink.sh" "$OUT/sion-linktest.sh" "$OUT/files/"*
