#!/bin/sh
# Installs the display keeper with Frolick's resolution switch
# (tools/sion/av/xbox-av.c: a mode in /run/xbox-av-mode is set when a
# title leaves, KMS_MODE takes WxHi) in /opt/xbcompat/bin, kept across
# reboots with sion persist, the old one kept as xbox-av.pre-settings.
# The running one is restarted: it takes the display back when the title
# on screen leaves (nothing changes on screen until then).
[ -f /tmp/xbox-av ] || exit 0
B=/opt/xbcompat/bin
[ -f "$B/xbox-av.pre-settings" ] || cp "$B/xbox-av" "$B/xbox-av.pre-settings"
mv /tmp/xbox-av "$B/xbox-av.new" && chmod 755 "$B/xbox-av.new" && mv "$B/xbox-av.new" "$B/xbox-av" || exit 1
sion persist "$B/xbox-av" "$B/xbox-av.pre-settings" >/dev/null
if pidof xbox-av >/dev/null; then
	killall xbox-av
	sleep 0.5
	[ -r /etc/default/kms ] && . /etc/default/kms
	export KMS_MODE XBOX_AV_SOCKET=/var/run/xbox-av.sock
	setsid "$B/xbox-av" >> /var/log/xbox-av.log 2>&1 < /dev/null &
	sleep 1
fi
pidof xbox-av >/dev/null && echo 'the display keeper is running (resolution switch)' || echo 'the display keeper is not running: tail /var/log/xbox-av.log'
