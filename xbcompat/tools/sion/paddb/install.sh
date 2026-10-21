#!/bin/sh
# Run on the Sion from this folder. Gives xbcompat's SDL the community
# controller database (github.com/mdqinc/SDL_GameControllerDB,
# gamecontrollerdb.txt), so pads SDL has no built-in layout for, such as the
# SideWinder Dual Strike, still get one and a port. Installs it in
# /opt/xbcompat/share and points SDL_GAMECONTROLLERCONFIG_FILE at it from
# /etc/default/xbox, live and in /data/overlay. uninstall.sh reverts.
# Restarts the dashboard.
set -e
cd "$(dirname "$0")"
DB=/opt/xbcompat/share/gamecontrollerdb.txt
LINE="export SDL_GAMECONTROLLERCONFIG_FILE=$DB  # sion paddb"
for r in "" /data/overlay; do
	mkdir -p $r/opt/xbcompat/share
	cp gamecontrollerdb.txt $r$DB
done
# The overlay's copy of /etc/default/xbox wins at boot, so the line goes
# into it too (starting from the live file if the overlay has none yet).
mkdir -p /data/overlay/etc/default
[ -f /data/overlay/etc/default/xbox ] || {
	[ -f /etc/default/xbox ] && cp /etc/default/xbox /data/overlay/etc/default/xbox
	touch /data/overlay/etc/default/.paddb-added
}
for f in /etc/default/xbox /data/overlay/etc/default/xbox; do
	grep -q '# sion paddb$' $f 2>/dev/null || echo "$LINE" >> $f
done
md5sum $DB; grep -n 'sion paddb' /etc/default/xbox /data/overlay/etc/default/xbox
/etc/init.d/S98xbox restart
