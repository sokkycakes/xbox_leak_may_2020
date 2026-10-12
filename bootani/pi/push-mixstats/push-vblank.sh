#!/bin/sh
# Linux side of "Pi vblank.cmd": sends the files to the Pi over ssh and runs
# vblank.sh there (pass "undo" to put the earlier build back). Saves the
# report as vblank.txt next to this script.
#   PI=192.168.1.128 sh push-vblank.sh [undo]
PI=${PI:-192.168.1.128}
D=$(cd "$(dirname "$0")" && pwd)
OPT="-o StrictHostKeyChecking=accept-new"
tar -cf - -C "$D" xbcompat-native.x86 libxbcompat_renderer.so.1 S02cpufreq clocks.sh vblank.sh |
	ssh $OPT root@$PI "rm -rf /tmp/push-vblank && mkdir -p /tmp/push-vblank && tar -xf - -C /tmp/push-vblank && sh /tmp/push-vblank/vblank.sh $1"
rc=$?
ssh $OPT root@$PI "cat /tmp/vblank.txt" > "$D/vblank.txt"
echo "Report: $D/vblank.txt"
exit $rc
