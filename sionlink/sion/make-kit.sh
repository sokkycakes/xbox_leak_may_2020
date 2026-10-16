#!/bin/sh
# Builds Sion Link and assembles the push kit in $1.
#   needs: BlockSDS (/opt/wonderful), devkitARM (/opt/devkitpro), makerom,
#          and a static hostapd at $HOSTAPD (built via build-hostapd.sh if unset)
set -e
OUT=${1:?usage: make-kit.sh OUTDIR}
here=$(cd "$(dirname "$0")" && pwd)
if [ -z "$HOSTAPD" ]; then
    if [ -x /tmp/sionlink-hostapd/hostapd-2.11/hostapd/hostapd ]; then
        HOSTAPD=/tmp/sionlink-hostapd/hostapd-2.11/hostapd/hostapd
    else
        HOSTAPD=$("$here/build-hostapd.sh")
    fi
fi
make -C "$here/../host"
if [ -n "$KDIR" ]; then
    make -C "$here/ratefix" KDIR="$KDIR" KBUILD_MODPOST_WARN=1
fi
PATH=/opt/wonderful/bin:$PATH make -C "$here/../ds"
PATH=/opt/wonderful/bin:$PATH make -C "$here/../demo"
PATH=/opt/wonderful/bin:$PATH make -C "$here/../linktest/ds"
make -C "$here/../linktest/sion"
if [ ! -d "$here/../ffcc/build/cli" ]; then
    python3 "$here/../ffcc/setup.py" ${FFCC_DISC:+--disc "$FFCC_DISC"} ${FFCC_DISC:---stub}
fi
make -C "$here/../ffcc/sion"
PATH=/opt/wonderful/bin:$PATH make -C "$here/../ffcc/ds"
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITARM=${DEVKITARM:-/opt/devkitpro/devkitARM} \
    make -C "$here/../ffcc/ctr" all cia
mkdir -p "$OUT/files"
cp "$here/../ds/sionlink.nds" "$here/../demo/sion-hello.nds" \
   "$here/../ffcc/ds/ffcc-gba.nds" "$here/../ffcc/ctr/ffcc-gba.cia" \
   "$here/../ffcc/ctr/ffcc-gba.3dsx" "$OUT/"
cp "$here/../host/sionlinkd" "$HOSTAPD" "$here/sion-dslink" \
   "$here/hostapd-sionlink.conf.in" "$here/install-sionlink.sh" \
   "$here/../linktest/sion/linktest" "$here/../linktest/ds/linktest.nds" \
   "$here/../ffcc/sion/ffhost" "$here/../ffcc/ds/ffcc-gba.nds" \
   "$here/../ffcc/ctr/ffcc-gba.cia" "$here/../ffcc/ctr/ffcc-gba.3dsx" \
   "$OUT/files/"
[ -f "$here/ratefix/iwl_ratefix.ko" ] && cp "$here/ratefix/iwl_ratefix.ko" "$OUT/files/"
KEYS_SRC=${KEYS_3DS:-}
[ -z "$KEYS_SRC" ] && [ -f "$here/3ds-keys.txt" ] && KEYS_SRC="$here/3ds-keys.txt"
[ -z "$KEYS_SRC" ] && [ -f /tmp/3ds-keys.txt ] && KEYS_SRC=/tmp/3ds-keys.txt
if [ -n "$KEYS_SRC" ] && [ -f "$KEYS_SRC" ]; then
    cp "$KEYS_SRC" "$OUT/files/3ds-keys.txt"
    chmod 600 "$OUT/files/3ds-keys.txt"
fi
strip "$OUT/files/hostapd" "$OUT/files/sionlinkd" "$OUT/files/linktest" "$OUT/files/ffhost"
cp "$here/sion-dslink.sh" "$here/sion-linktest.sh" "$here/sion-dslink.ps1" "$here/Sion DS link.cmd" "$OUT/"
chmod 755 "$OUT/sion-dslink.sh" "$OUT/sion-linktest.sh" "$OUT/files/hostapd" \
          "$OUT/files/sionlinkd" "$OUT/files/linktest" "$OUT/files/ffhost" \
          "$OUT/files/sion-dslink" "$OUT/files/install-sionlink.sh"
