#!/bin/bash
#
# One xbcompat for everything on the Sion: the Guide build with the Live work
# merged in (network card, NAT, saved settings). Installs it as
# /opt/xbcompat/bin/xbcompat with the Guide's assets, and points the Live
# dashboard at it (new launch-title and an HLE map made for this binary).
# Needs push-livedash and push-liveaccounts already installed for Live; games
# and the Guide don't. Run on the PC from a checkout of this branch:
#
#   bootani/sion/xbox/push-liveguide/install.sh             (ssh host "sion")
#   SION=root@192.168.1.247 bootani/sion/xbox/push-liveguide/install.sh
#
# Undo: /opt/xbcompat/bin/xbcompat.pre-live is the build it replaced, and
# launch-title falls back to the Live dashboard's own xbcompat by itself when
# the system one has no network card.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SION=${SION:-sion}

cd "$HERE"
md5sum -c --quiet MD5SUMS
# a Sion build loads its libraries through /opt/xbcompat/lib
grep -q /opt/xbcompat/lib/ld-linux.so.2 xbcompat || { echo 'xbcompat is not a Sion build (interpreter): not installing'; exit 1; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
K=$T/xbox-liveguide
mkdir -p "$K"
cp xbcompat sion-install.sh ../push-livedash/launch-title "$K/"
tar -xzf ../push-guide/guide.tar.gz -C "$K"
[ -f "$K/guide/hudbkgnd.xui" ] || { echo 'guide.tar.gz did not unpack to guide/'; exit 1; }

# BusyBox tar on the Sion has no -z.
(cd "$T" && tar -cf - xbox-liveguide) | gzip | ssh "$SION" \
    'rm -rf /tmp/xbox-liveguide && gunzip -c | tar -C /tmp -xf - && sh /tmp/xbox-liveguide/sion-install.sh'
