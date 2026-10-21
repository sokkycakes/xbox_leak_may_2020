#!/bin/bash
#
# xbcompat with Windows game cards: a Kazeta cart whose Runtime is windows-*
# shows up in the dashboard (stock and Frolick) as a title named after the
# cart, and launching it runs /opt/xbcompat/tools/wincart-run on the Sion
# (Wine from Conty under gamescope, with the Guide overlay host) instead of
# an Xbox title; quitting returns to the dashboard. Built from this branch's
# xbcompat (src/kernel/wincart.c) on top of the Live + Guide build (gudfcx).
# Run on the PC from a checkout of this branch:
#
#   bootani/sion/xbox/push-wincart/install.sh             (ssh host "sion")
#   SION=root@192.168.1.247 bootani/sion/xbox/push-wincart/install.sh
#
# Undo: /opt/xbcompat/bin/xbcompat.pre-wincart is the build it replaced.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SION=${SION:-sion}

cd "$HERE"
md5sum -c --quiet MD5SUMS
grep -q /opt/xbcompat/lib/ld-linux.so.2 xbcompat || { echo 'xbcompat is not a Sion build (interpreter): not installing'; exit 1; }
grep -q XBCOMPAT_WINCART_RUN xbcompat || { echo 'xbcompat has no Windows-cart support: not installing'; exit 1; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
K=$T/xbox-wincart
mkdir -p "$K"
cp xbcompat sion-install.sh "$K/"
[ -f ../push-livedash/launch-title ] && cp ../push-livedash/launch-title "$K/"

(cd "$T" && tar -cf - xbox-wincart) | gzip | ssh "$SION" \
    'rm -rf /tmp/xbox-wincart && gunzip -c | tar -C /tmp -xf - && sh /tmp/xbox-wincart/sion-install.sh'
