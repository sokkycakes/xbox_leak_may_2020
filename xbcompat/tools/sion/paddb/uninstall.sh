#!/bin/sh
# Run on the Sion: takes out what install.sh added and restarts the
# dashboard.
for r in "" /data/overlay; do
	rm -f $r/opt/xbcompat/share/gamecontrollerdb.txt
done
for f in /etc/default/xbox /data/overlay/etc/default/xbox; do
	[ -f $f ] && sed -i '/# sion paddb$/d' $f
done
if [ -f /data/overlay/etc/default/.paddb-added ]; then
	rm -f /data/overlay/etc/default/xbox /data/overlay/etc/default/.paddb-added
fi
echo removed
/etc/init.d/S98xbox restart
