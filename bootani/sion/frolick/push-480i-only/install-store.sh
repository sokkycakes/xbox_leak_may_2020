#!/bin/sh
# Installs Frolick's storefront helper on this Sion from the files copied
# to /tmp: frolick-store (/opt/frolick/bin) and S97frolick-store (started
# at boot), kept across reboots with sion persist, then (re)starts it.
# Downloads go to /data/frolick/downloads; the dashboard's side is
# E:\FROLICK\STORE (/data/xbox/hdd/partition1/FROLICK/STORE).
[ -f /tmp/frolick-store ] || exit 0
/etc/init.d/S97frolick-store stop 2>/dev/null
mkdir -p /opt/frolick/bin /data/frolick/downloads
mv /tmp/frolick-store /opt/frolick/bin/frolick-store && chmod 755 /opt/frolick/bin/frolick-store || exit 1
mv /tmp/S97frolick-store /etc/init.d/S97frolick-store && chmod 755 /etc/init.d/S97frolick-store || exit 1
sion persist /opt/frolick/bin/frolick-store /etc/init.d/S97frolick-store >/dev/null
/etc/init.d/S97frolick-store start
sleep 3
if pidof frolick-store >/dev/null; then
	echo 'the store helper is running'
	date -u '+the clock says %Y-%m-%d %H:%M UTC (archive.org needs it right)'
else
	echo 'the store helper is not running:'; tail -n 3 /var/log/frolick-store.log
fi
