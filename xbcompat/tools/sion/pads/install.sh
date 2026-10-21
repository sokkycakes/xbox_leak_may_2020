#!/bin/sh
# Run on the Sion from this folder: installs the extra controller drivers
# (see Makefile) live and in /data/overlay, so they come back after a
# reboot. uninstall.sh takes them out again.
set -e
cd "$(dirname "$0")"
for r in "" /data/overlay; do
	mkdir -p $r/opt/sion-pads $r/usr/sbin $r/etc/init.d $r/etc/udev/rules.d
	cp prebuilt/xpad_leds.ko prebuilt/hid-nintendo.ko $r/opt/sion-pads/
	cp sion-pads $r/usr/sbin/ && chmod 755 $r/usr/sbin/sion-pads
	cp S11pads $r/etc/init.d/ && chmod 755 $r/etc/init.d/S11pads
	cp 61-sion-pads.rules $r/etc/udev/rules.d/
done
udevadm control --reload 2>/dev/null || true
/usr/sbin/sion-pads start
sleep 2
echo "--- modules:"; grep -E '^(xpad_leds|hid_nintendo) ' /proc/modules || echo "(xpad_leds not loaded)"
echo "--- pads on xpad_leds:"; ls /sys/bus/usb/drivers/xpad_leds 2>/dev/null | grep : || echo "(none plugged in)"
echo "--- player lights:"; ls /sys/class/leds 2>/dev/null | grep xpad || echo "(none)"
echo "--- dmesg:"; dmesg | grep -iE 'xpad|unknown symbol|disagrees' | tail -15
md5sum /opt/sion-pads/*.ko
