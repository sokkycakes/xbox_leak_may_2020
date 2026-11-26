#!/bin/sh
# Runs on the Sion: back to the Xbox dashboard at boot.  Takes XBOX_DASH out
# of /etc/default/xbox; the devkit dashboard's files stay (install-devkit.sh
# turns it on again).  With "all" it also puts the session back as it was
# and deletes C:\devkit.
/etc/init.d/S98xbox stop > /dev/null
sleep 2
S=/usr/libexec/xbox-session
if [ -f /etc/default/xbox ]; then
	sed -i '/^XBOX_DASH=/d; /^# The devkit dashboard (C:\\devkit)/d' /etc/default/xbox
	sion persist /etc/default/xbox > /dev/null
fi
if [ "$1" = all ]; then
	if [ -f "$S.pre-devkit" ]; then
		mv "$S.pre-devkit" "$S"
		rm -f /data/overlay$S.pre-devkit
		sion persist "$S" > /dev/null
	fi
	rm -rf /opt/xbcompat/dash/devkit /data/overlay/opt/xbcompat/dash/devkit
	rm -f /usr/libexec/xbox-hostinfo /data/overlay/usr/libexec/xbox-hostinfo
	rm -f /usr/libexec/xbox-bluetooth /data/overlay/usr/libexec/xbox-bluetooth
	sync
fi
/etc/init.d/S98xbox start > /dev/null
sleep 8
pidof xbcompat > /dev/null && echo 'the Xbox dashboard is back' || echo 'the dashboard is not running (see /var/log/xbox.log)'
