#!/bin/sh
#
# Runs on the Sion (install.sh sends the kit, unpacked in /tmp/xbox-insignia):
#
#   /data/xbox-live/liveserver/    with --insignia, a DNS server pointing the
#                                  console at Insignia (insignia.py)
#   /data/xbox-live/launch-title   starts the server for $L/service, swaps
#                                  the hard disk's Live accounts per service
#                                  and gives games the network card
#   /data/xbox-live/service        "insignia"
#
# The test server's accounts move to /data/xbox/hdd/live-accounts/local the
# next time XBOX LIVE opens, and come back if service goes back to local.
K=/tmp/xbox-insignia
L=/data/xbox-live

[ -f $L/launch-title ] || { echo "the Live dashboard isn't installed (push-livedash)"; exit 1; }
for p in $(pidof xbcompat); do
	case $(tr '\0' ' ' < /proc/$p/cmdline) in
	*xonlinedash.xbe*) echo "XBOX LIVE is open: go back to the dashboard and run this again"; exit 1 ;;
	esac
done

PID=/var/run/xbox-live-server.pid
[ -s $PID ] && kill "$(cat $PID)" 2> /dev/null
rm -f $PID $PID.service
mkdir -p $L/liveserver
cp $K/liveserver/*.py $L/liveserver/
cp $K/launch-title $L/launch-title.new && chmod 755 $L/launch-title.new && mv $L/launch-title.new $L/launch-title
echo insignia > $L/service
sync
rm -rf $K
md5sum $L/launch-title $L/liveserver/liveserver.py $L/liveserver/insignia.py
echo "XBOX LIVE now signs in to Insignia"
