#!/bin/sh
# Installs the Frolick dashboard (built on the leak's dashboard engine) in
# place of the Xbox dashboard on this Sion, from the files copied to /tmp:
# xboxdash.xbe, xboxdash.map (its HLE symbol map) and default.xip.  The
# copies being replaced are kept as *.pre-frolick; revert-frolick.sh puts
# them back.  xbcompat itself and the other .xip files are not touched.
/etc/init.d/S98xbox stop >/dev/null
sleep 2
D=/opt/xbcompat/dash
keep=""
for f in xboxdash.xbe xboxdash.map default.xip; do
  [ -f "$D/$f" ] && [ ! -f "$D/$f.pre-frolick" ] && cp "$D/$f" "$D/$f.pre-frolick"
  keep="$keep $D/$f.pre-frolick"
  mv "/tmp/$f" "$D/$f.new" && chmod 644 "$D/$f.new" && mv "$D/$f.new" "$D/$f"
done
sion persist $D/xboxdash.xbe $D/xboxdash.map $D/default.xip $keep >/dev/null
/etc/init.d/S98xbox start >/dev/null
sleep 10
grep -E 'fatal|error' /var/log/xbox.log | tail -n 3
pidof xbcompat >/dev/null && echo 'the Frolick dashboard is running' || echo 'the dashboard is not running'
