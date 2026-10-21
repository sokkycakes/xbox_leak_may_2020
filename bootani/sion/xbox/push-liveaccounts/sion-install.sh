#!/bin/sh
#
# Runs on the Sion (install.sh sends it, unpacked in /tmp/xbox-live-accounts):
# replaces the Live server in /data/xbox-live/liveserver, stops the running
# one (launch-title starts the new one with the next XBOX LIVE), and adds the
# gamertags given as arguments to the hard disk and the server's accounts.
set -e
K=/tmp/xbox-live-accounts
L=/data/xbox-live
X=/opt/xbcompat
HDD=/data/xbox/hdd
PID=/var/run/xbox-live-server.pid

[ -f $L/launch-title ] || { echo "push-livedash isn't installed (no $L/launch-title)"; exit 1; }
if pidof xbcompat > /dev/null && ps w | grep -v grep | grep -q 'xonlinedash.xbe'; then
	echo "the Live dashboard is running: leave it (L + R + Back + Start) and run this again"
	exit 1
fi
if [ -s $PID ]; then
	kill "$(cat $PID)" 2> /dev/null || true
	rm -f $PID
fi
mkdir -p $L/liveserver
cp $K/liveserver/*.py $L/liveserver/
[ -f $L/live.json ] && cp $L/live.json $L/live.json.pre-accounts
# The bundled 32-bit python needs the xbox environment (its library path)
# outside the xbox services, e.g. over plain ssh.
[ -f /usr/lib/xbox/xbox-env ] && . /usr/lib/xbox/xbox-env
PYTHONHOME=$X PYTHONDONTWRITEBYTECODE=1 $X/bin/python3 $L/liveserver/mkuser.py --hdd $HDD --db $L/live.json "$@"
rm -rf $K
md5sum $L/liveserver/xweb.py $L/liveserver/sg.py $L/liveserver/kdc.py
echo "open XBOX LIVE on the dashboard to sign in"
