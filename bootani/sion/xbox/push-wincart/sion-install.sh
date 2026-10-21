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

/etc/init.d/S98xbox stop > /dev/null 2>&1

# The Windows-cart runner, its xbox-av helper, the Guide host and the carts'
# default overrides (the card itself is never written).
for f in wincart-run wincart-av; do
	cp $K/$f $X/tools/$f.new && chmod 755 $X/tools/$f.new && mv $X/tools/$f.new $X/tools/$f
done
cp $K/kazeta-guide $B/kazeta-guide.new && chmod 755 $B/kazeta-guide.new && mv $B/kazeta-guide.new $B/kazeta-guide
mkdir -p $X/share/wincart
rm -rf $X/share/wincart/overrides
cp -r $K/overrides $X/share/wincart/overrides
chmod -R a+rX $X/share/wincart
sion persist $X/tools/wincart-run $X/tools/wincart-av $B/kazeta-guide $X/share/wincart > /dev/null

# The extra modules (push-fuse, push-erofs, push-uinput) at boot: init.d
# scripts the overlay adds come too late for rcS, so S11modules loads them
# by name; the Sion has no depmod, so modules.dep gets their entries.
D=/lib/modules/$(uname -r)
for m in fuse erofs squashfs uinput; do
	[ -f $D/extra/$m.ko ] && ! grep -q "^extra/$m.ko:" $D/modules.dep && echo "extra/$m.ko:" >> $D/modules.dep
done
mkdir -p /etc/modules-load.d
cp $K/boot/sion-extra.conf /etc/modules-load.d/sion-extra.conf
sion persist $D/modules.dep /etc/modules-load.d/sion-extra.conf > /dev/null
for m in fuse erofs squashfs uinput; do grep -q "^extra/$m.ko:" $D/modules.dep && modprobe -q $m; done

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
md5sum $B/xbcompat $X/tools/wincart-run $X/tools/wincart-av $B/kazeta-guide
pidof xbcompat > /dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"
