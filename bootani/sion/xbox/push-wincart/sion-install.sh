#!/bin/sh
#
# Runs on the Sion (install.sh sends the kit, unpacked in /tmp/xbox-wincart):
#
#   /opt/xbcompat/bin/xbcompat     the Windows-cart build; the one it replaces
#                                  is kept as xbcompat.pre-wincart
#   /data/xbox-live/xodash.hlemap  remade for this binary when Live is installed
#
# The dashboard restarts. If the new xbcompat doesn't come up, the old one
# goes back.
K=/tmp/xbox-wincart
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
[ -x $X/tools/wincart-run ] || echo "note: $X/tools/wincart-run is missing; Windows carts will fail to start until it is installed"

/etc/init.d/S98xbox stop > /dev/null 2>&1
grep -q XBCOMPAT_WINCART_RUN $B/xbcompat || cp $B/xbcompat $B/xbcompat.pre-wincart
cp $K/xbcompat $B/xbcompat.new && chmod 755 $B/xbcompat.new && mv $B/xbcompat.new $B/xbcompat
sion persist $B/xbcompat $B/xbcompat.pre-wincart > /dev/null

if [ -f $L/xodash/xonlinedash.xbe ] && [ -f $L/launch-title ]; then
	# The Live dashboard's map is made for the binary that runs it.
	[ -f $L/xodash.hlemap.own ] || cp $L/xodash.hlemap $L/xodash.hlemap.own
	map=$(cd $X/tools && XBCOMPAT_BIN=$B/xbcompat PYTHONHOME=$X PYTHONDONTWRITEBYTECODE=1 $X/bin/python3 -c \
		'import sys, xbrun; print(xbrun.make_map(sys.argv[1]))' $L/xodash/xonlinedash.xbe 2> /tmp/xbox-wincart-map.log | tail -1)
	if [ -s "$map" ]; then
		cp "$map" $L/xodash.hlemap.new && mv $L/xodash.hlemap.new $L/xodash.hlemap
		sion persist $L/xodash.hlemap > /dev/null 2>&1
		echo "Live: map remade, $(wc -l < $L/xodash.hlemap) lines"
	else
		echo "Live: couldn't remake the map (see /tmp/xbox-wincart-map.log); the old map stays"
	fi
fi

/etc/init.d/S98xbox start > /dev/null 2>&1
sleep 10
if ! pidof xbcompat > /dev/null && [ -f $B/xbcompat.pre-wincart ]; then
	echo "the dashboard didn't start on the new xbcompat: putting the old one back"
	cp $B/xbcompat.pre-wincart $B/xbcompat && sion persist $B/xbcompat > /dev/null
	rm -f /var/run/xbox/dash-failed
	/etc/init.d/S99theseus stop > /dev/null 2>&1
	/etc/init.d/S98xbox start > /dev/null 2>&1
	sleep 10
fi
rm -rf $K
md5sum $B/xbcompat
pidof xbcompat > /dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"
