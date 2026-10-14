#!/bin/sh
# Linux side of request TT1 (Think Tanks): sends the files to the Pi over ssh and runs
# merge.sh there (pass "undo" to put the earlier build back). Saves the
# report as merge.txt next to this script.
#   PI=192.168.1.128 sh push-merge.sh [undo]
PI=${PI:-192.168.1.128}
D=$(cd "$(dirname "$0")" && pwd)
OPT="-o StrictHostKeyChecking=accept-new"
tar -cf - -C "$D" xbcompat-native.x86 libxbcompat_renderer.so.1 clocks.sh merge.sh |
	ssh $OPT root@$PI "rm -rf /tmp/push-merge && mkdir -p /tmp/push-merge && tar -xf - -C /tmp/push-merge && sh /tmp/push-merge/merge.sh $1"
rc=$?
ssh $OPT root@$PI "cat /tmp/merge.txt" > "$D/merge.txt"
echo "Report: $D/merge.txt"
exit $rc
