#!/bin/sh
# Linux side of bench request 3: sends the xlat build to the Pi over ssh and
# runs xlat.sh there (pass "undo" to put box86 back). Saves the report as
# xlat.txt next to this script.
#   PI=192.168.1.128 sh push-xlat.sh [undo]
PI=${PI:-192.168.1.128}
D=$(cd "$(dirname "$0")" && pwd)
OPT="-o StrictHostKeyChecking=accept-new -i $HOME/.ssh/id_ed25519_pi"
tar -cf - -C "$D" xbcompat.xlat xbprof xlat.sh |
	ssh $OPT root@$PI "rm -rf /tmp/push-xlat && mkdir -p /tmp/push-xlat && tar -xf - -C /tmp/push-xlat && sh /tmp/push-xlat/xlat.sh $1"
rc=$?
ssh $OPT root@$PI "cat /tmp/xlat.txt" > "$D/xlat.txt"
echo "Report: $D/xlat.txt"
exit $rc
