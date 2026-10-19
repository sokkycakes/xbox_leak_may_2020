#!/bin/bash
#
# Puts the Xbox Live dashboard on the Sion, beside the normal one, in
# /data/xbox-live (see live-dash). Run on the PC from a checkout of this
# branch; the dashboard's media come from the leak in the same checkout:
#
#   bootani/sion/xbox/push-livedash/install.sh            (ssh host "sion")
#   SION=root@192.168.1.247 bootani/sion/xbox/push-livedash/install.sh
#
# It leaves /opt/xbcompat and /data/xbox alone, and keeps the Live hard disk
# and server accounts of an earlier install. Start it on the Sion with
# /data/xbox-live/live-dash (that stops the normal dashboard until you leave
# with L + R + Back + Start).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
SION=${SION:-sion}
S="$REPO/xbox_leak_may_2020/xbox trunk/xbox/private/online/xodash"

cd "$HERE"
md5sum -c --quiet MD5SUMS

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
K=$T/xbox-live
Y=$K/hdd/partition2
mkdir -p "$K/bin" "$K/liveserver" "$Y/media" "$Y/audio"
cp xbcompat "$K/bin/"
cp live-dash xodash.hlemap "$K/"
chmod 755 "$K/bin/xbcompat" "$K/live-dash"
cp xodash.xbe "$Y/"
cp "$REPO"/xbcompat/tools/liveserver/*.py "$K/liveserver/"
# The layout tools/xodashbuild/build.sh stages (copymedia.bat's).
cp "$S"/*.xtf "$Y/"
cp -r "$S/audio/mainaudio" "$Y/audio/"
for d in Xbg Xbx Content; do cp -r "$S/Media/$d" "$Y/media/"; done
cp "$S"/Media/*.csv "$Y/media/"

echo "copying $(du -sh "$K" | cut -f1) to $SION:/data/xbox-live"
if [ "${SION#local:}" != "$SION" ]; then
    mkdir -p "${SION#local:}" && (cd "$T" && tar -cf - xbox-live) | tar -C "${SION#local:}" -xf -
    exit 0
fi
# BusyBox tar on the Sion has no -z.
(cd "$T" && tar -cf - xbox-live) | gzip | ssh "$SION" \
    'mkdir -p /data && gunzip -c | tar -C /data -xf - && md5sum /data/xbox-live/bin/xbcompat /data/xbox-live/hdd/partition2/xodash.xbe && du -sh /data/xbox-live'
echo "installed; start it on the Sion with /data/xbox-live/live-dash"
