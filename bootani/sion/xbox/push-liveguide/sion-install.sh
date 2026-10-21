#!/bin/sh
#
# Runs on the Sion (install.sh sends the kit, unpacked in /tmp/xbox-liveguide):
#
#   /opt/xbcompat/bin/xbcompat     the merged build; the one it replaces, if it
#                                  had no network card, is kept as xbcompat.pre-live
#   /opt/xbcompat/bin/guide        the Guide's assets
#   /data/xbox-live/launch-title   runs the Live dashboard on /opt's xbcompat
#   /data/xbox-live/xodash.hlemap  the Live dashboard's map, made for that
#                                  binary; the map for the Live dashboard's own
#                                  xbcompat is kept as xodash.hlemap.own
#
# The dashboard restarts. If the new xbcompat doesn't come up, the old one
# goes back.
K=/tmp/xbox-liveguide
L=/data/xbox-live
X=/opt/xbcompat
B=$X/bin
[ -f /usr/lib/xbox/xbox-env ] && . /usr/lib/xbox/xbox-env

for p in $(pidof xbcompat); do
	case $(tr '\0' ' ' < /proc/$p/cmdline) in
	*xboxdash.xbe*) ;;
	*) echo "a game or the Live dashboard is running: go back to the dashboard and run this again"; exit 1 ;;
	esac
done

/etc/init.d/S98xbox stop > /dev/null 2>&1
grep -q XBCOMPAT_DNS $B/xbcompat || cp $B/xbcompat $B/xbcompat.pre-live
cp $K/xbcompat $B/xbcompat.new && chmod 755 $B/xbcompat.new && mv $B/xbcompat.new $B/xbcompat
rm -rf $B/guide.new && mv $K/guide $B/guide.new && rm -rf $B/guide && mv $B/guide.new $B/guide
sion persist $B/xbcompat $B/xbcompat.pre-live $B/guide > /dev/null

if [ -f $L/xodash/xonlinedash.xbe ]; then
	[ -f $L/xodash.hlemap.own ] || cp $L/xodash.hlemap $L/xodash.hlemap.own
	# The Sion's xbrun.py has no --map-only; make_map() names the map it made.
	map=$(cd $X/tools && XBCOMPAT_BIN=$B/xbcompat PYTHONHOME=$X PYTHONDONTWRITEBYTECODE=1 $X/bin/python3 -c \
		'import sys, xbrun; print(xbrun.make_map(sys.argv[1]))' $L/xodash/xonlinedash.xbe 2> /tmp/xbox-liveguide-map.log | tail -1)
	if [ -s "$map" ]; then
		cp "$map" $L/xodash.hlemap.new && mv $L/xodash.hlemap.new $L/xodash.hlemap
		cp $K/launch-title $L/launch-title.new && chmod 755 $L/launch-title.new && mv $L/launch-title.new $L/launch-title
		echo "Live: runs on $B/xbcompat, map $(wc -l < $L/xodash.hlemap) lines"
	else
		echo "Live: couldn't make the map (see /tmp/xbox-liveguide-map.log); Live keeps its own xbcompat"
	fi
fi

/etc/init.d/S98xbox start > /dev/null 2>&1
sleep 10
if ! pidof xbcompat > /dev/null && [ -f $B/xbcompat.pre-live ]; then
	echo "the dashboard didn't start on the new xbcompat: putting the old one back"
	cp $B/xbcompat.pre-live $B/xbcompat && sion persist $B/xbcompat > /dev/null
	rm -f /var/run/xbox/dash-failed
	/etc/init.d/S99theseus stop > /dev/null 2>&1
	/etc/init.d/S98xbox start > /dev/null 2>&1
	sleep 10
fi
rm -rf $K
md5sum $B/xbcompat $L/launch-title
pidof xbcompat > /dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"
