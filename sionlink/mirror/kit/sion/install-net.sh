#!/bin/sh
# Sion side of the RG DS mirror: DS titles serve the DSpico and the network at once
# (mirrorrun --serve usb,tcp:7790), so the RG DS's "Sion DS Mirror" can show them.
# Run on the Sion as root from this folder. Refuses while a game runs.
cd "$(dirname "$0")" || exit 1
for p in xbcompat xbcompat-gpu mirrorrun mirrorrun-gl; do
    pidof $p >/dev/null && { echo "a game is running ($p): not installing now"; exit 1; }
done
B=/opt/sionlink
[ -d $B ] || { echo "no $B: install a DS title first (install-title.sh)"; exit 1; }
stamp=$(date +%Y%m%d-%H%M)
for f in mirrorrun mirrorrun-gl dscart-run; do
    [ -f $B/$f ] && cp -p $B/$f $B/$f.pre-net-$stamp
    cp $f $B/$f && chmod +x $B/$f
done
sion persist $B
echo "installed: DS titles now take a DS on the DSpico or the RG DS over the network (port 7790)"
echo "backups: $B/*.pre-net-$stamp"
