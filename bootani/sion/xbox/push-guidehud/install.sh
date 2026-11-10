#!/bin/bash
#
# push-guidehud (2026-11-10): push-compat27 with the Guide drawn into its own
# HUD buffer, as the console's XAM does (xbcompat and kazeta-guide).
#
# The Sion's combined xbcompat (Guide, Live, Windows game cards, streamed
# discs, the GPU-build switch: xujlbp f4ae70a1) with the 2026-10-26/27
# compatibility passes merged in (branch claude/project-thread-di7q3m),
# 2026-10-27 evening: plus the Hulk intro fix, the Halo 2 second-device
# window fix, and the game-matching
# signatures (tools/xbsymmap.py) those passes added, which Halo 2 needs.
# The Guide, kazeta-guide and launch-title are the ones already installed.
# Refuses while a game is running. Run on the PC:
#
#   bootani/sion/xbox/push-guidehud/install.sh             (ssh host "sion")
#
# Undo: /opt/xbcompat/bin/xbcompat.pre-guidehud and kazeta-guide.prev are the
# builds it replaced:
#   ssh sion 'cd /opt/xbcompat/bin && cp xbcompat.pre-guidehud xbcompat && cp kazeta-guide.prev kazeta-guide && sion persist xbcompat kazeta-guide'
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SION=${SION:-sion}

cd "$HERE"
md5sum -c --quiet MD5SUMS
grep -q /opt/xbcompat/lib/ld-linux.so.2 xbcompat || { echo 'xbcompat is not a Sion build (interpreter): not installing'; exit 1; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
K=$T/xbox-guidehud
mkdir -p "$K"
cp xbcompat sion-install.sh launch-title kazeta-guide xbsymmap.py "$K/"
tar -xzf guide.tar.gz -C "$K"
[ -f "$K/guide/hudbkgnd.xui" ] || { echo 'guide.tar.gz did not unpack to guide/'; exit 1; }

# a backup that later installs don't overwrite
ssh "$SION" '[ -f /opt/xbcompat/bin/xbcompat.pre-guidehud ] || { cp /opt/xbcompat/bin/xbcompat /opt/xbcompat/bin/xbcompat.pre-guidehud && sion persist /opt/xbcompat/bin/xbcompat.pre-guidehud > /dev/null; }; md5sum /opt/xbcompat/bin/xbcompat'
(cd "$T" && tar -cf - xbox-guidehud) | gzip | ssh "$SION" \
    'rm -rf /tmp/xbox-guidehud && gunzip -c | tar -C /tmp -xf - && sh /tmp/xbox-guidehud/sion-install.sh'
echo "expected: $(md5sum < xbcompat | cut -c1-32)  /opt/xbcompat/bin/xbcompat"
echo "expected: $(md5sum < kazeta-guide | cut -c1-32)  /opt/xbcompat/bin/kazeta-guide"
echo "expected: $(md5sum < xbsymmap.py | cut -c1-32)  /opt/xbcompat/tools/xbsymmap.py"
