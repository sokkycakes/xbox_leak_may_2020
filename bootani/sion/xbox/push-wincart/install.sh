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
#   QUICK=1 bootani/sion/xbox/push-wincart/install.sh   (only the runner and
#                       the Guide host; xbcompat and the dashboard are left alone)
#
# It also installs the Sion side: wincart-run and wincart-av (tools), the
# kazeta-guide overlay host (bin), the carts' default overrides (share/wincart)
# and the boot loading of the fuse/erofs/squashfs/uinput modules; see README.
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
cp xbcompat sion-install.sh wincart-run wincart-av "$K/"
cp kazeta-guide/kazeta-guide drmkeep/drmkeep.so "$K/"
cp -r overrides boot "$K/"
[ -f ../push-livedash/launch-title ] && cp ../push-livedash/launch-title "$K/"

(cd "$T" && tar -cf - xbox-wincart) | gzip | ssh "$SION" \
    "rm -rf /tmp/xbox-wincart && gunzip -c | tar -C /tmp -xf - && QUICK=${QUICK:-0} sh /tmp/xbox-wincart/sion-install.sh"
