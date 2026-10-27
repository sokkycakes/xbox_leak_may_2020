#!/bin/bash
#
# The Sion's combined xbcompat (Guide, Live, Windows game cards, streamed
# discs, the GPU-build switch: xujlbp f4ae70a1) with the 2026-10-26/27
# compatibility passes merged in (branch claude/project-thread-di7q3m).
# The Guide, kazeta-guide and launch-title are the ones already installed.
# Refuses while a game is running. Run on the PC:
#
#   bootani/sion/xbox/push-compat27/install.sh             (ssh host "sion")
#
# Undo: /opt/xbcompat/bin/xbcompat.pre-compat27 is the build it replaced:
#   ssh sion 'cp /opt/xbcompat/bin/xbcompat.pre-compat27 /opt/xbcompat/bin/xbcompat && sion persist /opt/xbcompat/bin/xbcompat'
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SION=${SION:-sion}

cd "$HERE"
md5sum -c --quiet MD5SUMS
grep -q /opt/xbcompat/lib/ld-linux.so.2 xbcompat || { echo 'xbcompat is not a Sion build (interpreter): not installing'; exit 1; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
K=$T/xbox-compat27
mkdir -p "$K"
cp xbcompat sion-install.sh launch-title kazeta-guide "$K/"
tar -xzf guide.tar.gz -C "$K"
[ -f "$K/guide/hudbkgnd.xui" ] || { echo 'guide.tar.gz did not unpack to guide/'; exit 1; }

# a backup that later installs don't overwrite
ssh "$SION" '[ -f /opt/xbcompat/bin/xbcompat.pre-compat27 ] || { cp /opt/xbcompat/bin/xbcompat /opt/xbcompat/bin/xbcompat.pre-compat27 && sion persist /opt/xbcompat/bin/xbcompat.pre-compat27 > /dev/null; }; md5sum /opt/xbcompat/bin/xbcompat'
(cd "$T" && tar -cf - xbox-compat27) | gzip | ssh "$SION" \
    'rm -rf /tmp/xbox-compat27 && gunzip -c | tar -C /tmp -xf - && sh /tmp/xbox-compat27/sion-install.sh'
echo "expected: $(md5sum < xbcompat | cut -c1-32)  /opt/xbcompat/bin/xbcompat"
