#!/bin/bash
#
# Puts the Xbox Live dashboard on the Sion and an XBOX LIVE item on the
# normal dashboard's main menu that starts it (see sion-install.sh for what
# changes there). Run on the PC from a checkout of this branch; the Live
# dashboard's media come from the leak in the same checkout:
#
#   bootani/sion/xbox/push-livedash/install.sh            (ssh host "sion")
#   SION=root@192.168.1.247 bootani/sion/xbox/push-livedash/install.sh
#
# Leave the Live dashboard with L + R + Back + Start.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
SION=${SION:-sion}
S="$REPO/xbox_leak_may_2020/xbox trunk/xbox/private/online/xodash"

cd "$HERE"
md5sum -c --quiet MD5SUMS

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
K=$T/xbox-live-kit/xbox-live
Y=$K/xodash
mkdir -p "$K/bin" "$K/liveserver" "$Y/media" "$Y/audio" "$T/xbox-live-kit/dash"
cp xbcompat "$K/bin/"
cp launch-title xodash.hlemap "$K/"
cp xodash.xbe "$Y/xonlinedash.xbe"
cp "$REPO"/xbcompat/tools/liveserver/*.py "$K/liveserver/"
# The layout tools/xodashbuild/build.sh stages (copymedia.bat's).
cp "$S"/*.xtf "$Y/"
cp -r "$S/audio/mainaudio" "$Y/audio/"
for d in Xbg Xbx Content; do cp -r "$S/Media/$d" "$Y/media/"; done
cp "$S"/Media/*.csv "$Y/media/"
cp dash/* "$T/xbox-live-kit/dash/"
cp sion-install.sh "$T/xbox-live-kit/"

echo "copying $(du -sh "$T/xbox-live-kit" | cut -f1) to $SION"
if [ "${SION#local:}" != "$SION" ]; then
    mkdir -p "${SION#local:}" && (cd "$T" && tar -cf - xbox-live-kit) | tar -C "${SION#local:}" -xf -
    exit 0
fi
# BusyBox tar on the Sion has no -z.
(cd "$T" && tar -cf - xbox-live-kit) | gzip | ssh "$SION" \
    'rm -rf /tmp/xbox-live-kit && gunzip -c | tar -C /tmp -xf - && sh /tmp/xbox-live-kit/sion-install.sh'
