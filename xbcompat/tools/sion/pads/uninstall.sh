#!/bin/sh
# Run on the Sion: puts every pad back on the built-in xpad and removes
# what install.sh added, live and from /data/overlay.
/usr/sbin/sion-pads stop 2>/dev/null
for r in "" /data/overlay; do
	rm -rf $r/opt/sion-pads
	rm -f $r/usr/sbin/sion-pads $r/etc/init.d/S11pads $r/etc/udev/rules.d/61-sion-pads.rules
done
udevadm control --reload 2>/dev/null || true
echo "removed"; grep -E '^(xpad_leds|hid_nintendo) ' /proc/modules || true
