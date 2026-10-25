#!/bin/sh
# Linux PC side: streams this folder to the Pi over ssh (the Pi has no sftp,
# so no scp) and runs xbt-test.sh there. Pass "undo" to delete /data/xbt
# from the Pi. Saves the report as xbt-report.txt next to this script.
#   PI=192.168.1.128 sh push-xbt.sh [undo]
PI=${PI:-192.168.1.128}
D=$(cd "$(dirname "$0")" && pwd)
OPT="-o StrictHostKeyChecking=accept-new"
[ -f ~/.ssh/id_ed25519_pi ] && OPT="$OPT -i $HOME/.ssh/id_ed25519_pi"
if [ "$1" = undo ]; then
	ssh $OPT root@$PI "sh /data/xbt/xbt-test.sh undo || rm -rf /data/xbt"
	exit $?
fi
tar -cf - -C "$D" xbt xbt-test.sh bench recovery |
	ssh $OPT root@$PI "rm -rf /tmp/push-xbt && mkdir -p /tmp/push-xbt && tar -xf - -C /tmp/push-xbt && sh /tmp/push-xbt/xbt-test.sh; rm -rf /tmp/push-xbt"
rc=$?
ssh $OPT root@$PI "cat /tmp/xbt-report.txt" > "$D/xbt-report.txt"
echo "Report: $D/xbt-report.txt"
exit $rc
