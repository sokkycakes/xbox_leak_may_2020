#!/bin/sh
# Run on the Sion from this folder. Installs the Xbox dashboard build that
# orders game card carts by an optional Order= line in each .kzi. The
# replaced files are kept as *.pre-order. xbcompat is not touched.
set -e
cd "$(dirname "$0")"
D=/opt/xbcompat/dash
md5sum -c MD5SUMS
/etc/init.d/S98xbox stop >/dev/null
sleep 2
keep=""
for f in xboxdash.xbe xboxdash.map; do
	[ -f "$D/$f" ] && [ ! -f "$D/$f.pre-order" ] && cp "$D/$f" "$D/$f.pre-order"
	keep="$keep $D/$f.pre-order"
	cp "$f" "$D/$f.new" && chmod 644 "$D/$f.new" && mv "$D/$f.new" "$D/$f"
done
sion persist $D/xboxdash.xbe $D/xboxdash.map $keep >/dev/null
/etc/init.d/S98xbox start >/dev/null
sleep 10
grep -E 'fatal|error' /var/log/xbox.log | tail -n 3 || true
(cd $D && md5sum xboxdash.xbe xboxdash.map)
pidof xbcompat >/dev/null && echo 'the dashboard is running' || echo 'the dashboard is not running'
