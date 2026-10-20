#!/bin/sh
# Puts the Xbox dashboard back from the *.pre-frolick copies.
/etc/init.d/S98xbox stop >/dev/null
sleep 2
D=/opt/xbcompat/dash
for f in xboxdash.xbe xboxdash.map default.xip; do
  [ -f "$D/$f.pre-frolick" ] && mv "$D/$f.pre-frolick" "$D/$f"
done
sion persist $D/xboxdash.xbe $D/xboxdash.map $D/default.xip >/dev/null
/etc/init.d/S98xbox start >/dev/null
sleep 10
pidof xbcompat >/dev/null && echo 'the Xbox dashboard is back' || echo 'the dashboard is not running'
