#!/bin/sh
#
# Runs on the Sion (install.sh sends the kit, unpacked in /tmp/xbox-compat27):
#
#   /opt/xbcompat/bin/xbcompat     the merged build; the one it replaces is kept as
#                                  xbcompat.prev (and, if it had no network card,
#                                  as xbcompat.pre-live)
#   /opt/xbcompat/bin/guide        the Guide's assets
#   /opt/xbcompat/bin/kazeta-guide the Windows cards' Guide (old one kept as
#                                  kazeta-guide.prev)
#   /opt/xbcompat/tools/xbsymmap.py the game-matching signatures the build's fixes
#                                  rely on (old one kept as xbsymmap.py.pre-compat27);
#                                  maps are remade on each game's next launch
#   /data/xbox-live/launch-title   runs the Live dashboard on /opt's xbcompat
#   /data/xbox-live/xodash.hlemap  the Live dashboard's map, made for that
#                                  binary; the map for the Live dashboard's own
#                                  xbcompat is kept as xodash.hlemap.own
#
# The dashboard restarts. If the new xbcompat doesn't come up, the old one
# goes back.
K=/tmp/xbox-compat27
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
cp $B/xbcompat $B/xbcompat.prev
cp $K/xbcompat $B/xbcompat.new && chmod 755 $B/xbcompat.new && mv $B/xbcompat.new $B/xbcompat
rm -rf $B/guide.prev && cp -a $B/guide $B/guide.prev 2> /dev/null
rm -rf $B/guide.new && mv $K/guide $B/guide.new && rm -rf $B/guide && mv $B/guide.new $B/guide
if [ -f $K/kazeta-guide ]; then
	[ -f $B/kazeta-guide ] && cp $B/kazeta-guide $B/kazeta-guide.prev
	cp $K/kazeta-guide $B/kazeta-guide.new && chmod 755 $B/kazeta-guide.new && mv $B/kazeta-guide.new $B/kazeta-guide
	sion persist $B/kazeta-guide $B/kazeta-guide.prev > /dev/null 2>&1
fi
if [ -f $K/xbsymmap.py ]; then
	T=$X/tools/xbsymmap.py
	[ -f $T.pre-compat27 ] || { cp $T $T.pre-compat27 && sion persist $T.pre-compat27 > /dev/null 2>&1; }
	cp $K/xbsymmap.py $T.new && mv $T.new $T && sion persist $T > /dev/null 2>&1
fi
keep=
for f in $B/xbcompat.pre-live $B/guide.prev; do [ -e $f ] && keep="$keep $f"; done
sion persist $B/xbcompat $B/xbcompat.prev $B/guide $keep > /dev/null

if [ -f $L/xodash/xonlinedash.xbe ]; then
	[ -f $L/xodash.hlemap.own ] || cp $L/xodash.hlemap $L/xodash.hlemap.own
	# The Sion's xbrun.py has no --map-only; make_map() names the map it made.
	map=$(cd $X/tools && XBCOMPAT_BIN=$B/xbcompat PYTHONHOME=$X PYTHONDONTWRITEBYTECODE=1 $X/bin/python3 -c \
		'import sys, xbrun; print(xbrun.make_map(sys.argv[1]))' $L/xodash/xonlinedash.xbe 2> /tmp/xbox-compat27-map.log | tail -1)
	if [ -s "$map" ]; then
		cp "$map" $L/xodash.hlemap.new && mv $L/xodash.hlemap.new $L/xodash.hlemap
		cp $K/launch-title $L/launch-title.new && chmod 755 $L/launch-title.new && mv $L/launch-title.new $L/launch-title
		echo "Live: runs on $B/xbcompat, map $(wc -l < $L/xodash.hlemap) lines"
	else
		echo "Live: couldn't make the map (see /tmp/xbox-compat27-map.log); Live keeps its own xbcompat"
	fi
fi

/etc/init.d/S98xbox start > /dev/null 2>&1
sleep 10
if ! pidof xbcompat > /dev/null && [ -f $B/xbcompat.prev ]; then
	echo "the dashboard didn't start on the new xbcompat: putting the old one back"
	cp $B/xbcompat.prev $B/xbcompat && sion persist $B/xbcompat > /dev/null
	rm -f /var/run/xbox/dash-failed
	/etc/init.d/S99theseus stop > /dev/null 2>&1
	/etc/init.d/S98xbox start > /dev/null 2>&1
	sleep 10
fi
rm -rf $K
md5sum $B/xbcompat $B/kazeta-guide $L/launch-title $X/tools/xbsymmap.py
pidof xbcompat > /dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"
