#!/bin/sh
#
# Runs on the Sion, from the folder it was copied to:
#
#   trigger6.ko  -> /lib/modules/6.18.7/extra   (USB display adapters with an
#                   MCT Trigger 6 chip: j5create JUA365 dual HDMI and friends),
#                   loaded now and at boot by S11modules
#   t6-pattern   -> /usr/bin                    (draws a test pattern on the
#                   adapter's outputs)
#
# The adapter becomes its own DRM card (/dev/dri/cardN, driver "trigger6");
# the Chromebox's own HDMI stays card0, so the dashboard is not touched and
# nothing is restarted. TEST=1 draws colour bars on every connected adapter
# output for 10 seconds each (skipped while a game is running).
set -e
K=$(cd "$(dirname "$0")" && pwd)
D=/lib/modules/$(uname -r)

mkdir -p $D/extra
cp $K/trigger6.ko $D/extra/trigger6.ko
cp $K/t6-pattern /usr/bin/t6-pattern
chmod 755 /usr/bin/t6-pattern
[ -e $D/modules.dep.pre-trigger6 ] || cp -a $D/modules.dep $D/modules.dep.pre-trigger6
grep -q '^extra/trigger6.ko:' $D/modules.dep || echo 'extra/trigger6.ko:' >> $D/modules.dep
C=/etc/modules-load.d/sion-extra.conf
mkdir -p /etc/modules-load.d
grep -qx trigger6 $C 2>/dev/null || { echo '# MCT Trigger 6 USB display adapters (j5create JUA365)'; echo trigger6; } >> $C
sion persist $D/extra/trigger6.ko $D/modules.dep $C /usr/bin/t6-pattern > /dev/null

if grep -q '^trigger6 ' /proc/modules; then
	echo "trigger6 was already loaded: reloading"
	rmmod trigger6 || echo "could not unload the old trigger6 (in use?); reboot to pick up the new one"
fi
modprobe trigger6 || insmod $D/extra/trigger6.ko
sleep 3

card=
for c in /sys/class/drm/card[0-9]; do
	[ "$(basename "$(readlink -f "$c/device/driver" 2>/dev/null)")" = trigger6 ] && card=$c
done
if [ -z "$card" ]; then
	echo "trigger6 loaded, but no adapter found. Is the JUA365 plugged in? (lsusb should show 0711:5601)"
	dmesg | grep -i trigger6 | tail -5
	exit 0
fi
echo "adapter: /dev/dri/$(basename $card)"
for s in $card-*/status; do echo "  $(basename "$(dirname "$s")"): $(cat "$s")"; done
dmesg | grep -i trigger6 | tail -5

if [ "${TEST:-0}" = 1 ]; then
	playing=
	for p in $(pidof xbcompat); do
		tr '\0' ' ' < "/proc/$p/cmdline" | grep -q -e xboxdash -e Frolick -e Bootscreen || playing=yes
	done
	pidof gamescope kazeta-guide mirrorrun mirrorrun-gl > /dev/null && playing=yes
	if [ -n "$playing" ]; then
		echo "a game is running: not drawing the test pattern"
		exit 0
	fi
	i=0
	for s in $card-*/status; do
		if [ "$(cat "$s")" = connected ]; then
			echo "test pattern on $(basename "$(dirname "$s")") for 10 s"
			t6-pattern /dev/dri/$(basename $card) $i 10 || true
		fi
		i=$((i + 1))
	done
fi
exit 0
