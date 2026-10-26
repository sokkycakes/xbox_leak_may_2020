#!/bin/bash
#
# 15 kHz 480i for a VGA-to-composite transcoder (see ../README.md): sion-video
# and its EDID, the hook that runs it when the dashboard starts, and the
# Windows-game runner's fix for the 2880x480i mode name. Run on the PC from a
# checkout of this branch:
#
#   bootani/sion/video/push-480i/install.sh                (ssh host "sion")
#   SION=root@192.168.1.247 bootani/sion/video/push-480i/install.sh
#   VIDEO=480i ...   force 480i (default auto: 480p for the HDMI-to-AV box
#                    plugged in now, 480i for anything else)
#   NOW=1 ...        restart the dashboard so it applies straight away (not
#                    while a game runs)
#
# Undo: ssh sion /usr/libexec/sion-video 480p (and restart the dashboard).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SION=${SION:-sion}
R=$HERE/../../../buildroot/package/sion-tools
[ -f "$HERE/sion-video" ] && R=$HERE   # the copy in the project files carries its own

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
K=$T/sion-480i
mkdir -p "$K"
cp "$HERE/sion-install.sh" "$R/sion-video" "$R/crimson-480i.edid" "$K/"
(cd "$T" && tar -cf - sion-480i) | gzip | ssh "$SION" \
    "rm -rf /tmp/sion-480i && gunzip -c | tar -C /tmp -xf - && VIDEO=${VIDEO:-} NOW=${NOW:-0} sh /tmp/sion-480i/sion-install.sh"
