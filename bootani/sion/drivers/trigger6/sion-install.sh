#!/bin/sh
#
# Runs on the Sion, from the folder it was copied to:
#
#   trigger6.ko  -> /lib/modules/6.18.7/extra   (USB display adapters with an
#                   MCT Trigger 6 chip: j5create JUA365 dual HDMI and friends),
#                   loaded now and at boot by S11modules
#   t6-pattern   -> /usr/bin                    (draws a test pattern on the
#                   adapter's outputs)
#                   The driver also adds an ALSA card "Trigger6" for the
#                   adapter's HDMI audio (TEST=1 plays a 2 s tone on it).
#   t6-mirror    -> /usr/bin, S99t6mirror -> /etc/init.d (shows the main
#                   screen on the adapter at 720x480p whenever it's plugged in;
#                   started at boot through /etc/sion/late-start)
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
# The mirror holds the adapter open: stop it (and any test copy) first.
[ -x /etc/init.d/S99t6mirror ] && /etc/init.d/S99t6mirror stop
killall t6-mirror 2>/dev/null && sleep 1
cp $K/t6-pattern /usr/bin/t6-pattern
cp $K/t6-mirror /usr/bin/t6-mirror
cp $K/S99t6mirror /etc/init.d/S99t6mirror
chmod 755 /usr/bin/t6-pattern /usr/bin/t6-mirror /etc/init.d/S99t6mirror
L=/etc/sion/late-start
mkdir -p /etc/sion
grep -qx S99t6mirror $L 2>/dev/null || echo S99t6mirror >> $L
[ -e $D/modules.dep.pre-trigger6 ] || cp -a $D/modules.dep $D/modules.dep.pre-trigger6
grep -q '^extra/trigger6.ko:' $D/modules.dep || echo 'extra/trigger6.ko:' >> $D/modules.dep
C=/etc/modules-load.d/sion-extra.conf
mkdir -p /etc/modules-load.d
grep -qx trigger6 $C 2>/dev/null || { echo '# MCT Trigger 6 USB display adapters (j5create JUA365)'; echo trigger6; } >> $C
sion persist $D/extra/trigger6.ko $D/modules.dep $C /usr/bin/t6-pattern /usr/bin/t6-mirror \
	/etc/init.d/S99t6mirror $L > /dev/null

# The sound router (push-soundrouter) holds the adapter's sound card.
ROUTER=
pidof sion-audiod > /dev/null && ROUTER=1 && { /etc/init.d/S07sion-audiod stop || killall sion-audiod; sleep 1; } || true
if grep -q '^trigger6 ' /proc/modules; then
	echo "trigger6 was already loaded: reloading"
	rmmod trigger6 || echo "could not unload the old trigger6 (in use?); reboot to pick up the new one"
fi
modprobe trigger6 || insmod $D/extra/trigger6.ko
sleep 3
[ -n "$ROUTER" ] && /etc/init.d/S07sion-audiod start || true

card=
for c in /sys/class/drm/card[0-9]; do
	[ "$(basename "$(readlink -f "$c/device/driver" 2>/dev/null)")" = trigger6 ] && card=$c
done
if [ -z "$card" ]; then
	echo "trigger6 loaded, but no adapter found. Is the JUA365 plugged in? (lsusb should show 0711:5601)"
	dmesg | grep -i trigger6 | tail -5
	/etc/init.d/S99t6mirror start
	echo "the mirror will start by itself when the adapter is plugged in"
	exit 0
fi
echo "adapter: /dev/dri/$(basename $card)"
for s in $card-*/status; do echo "  $(basename "$(dirname "$s")"): $(cat "$s")"; done
dmesg | grep -i trigger6 | tail -5

if [ "${TEST:-0}" = 1 ]; then
	playing=
	for p in $(pidof xbcompat xbcompat-gpu); do
		tr '\0' ' ' < "/proc/$p/cmdline" | grep -q -e xboxdash -e Frolick -e Bootscreen || playing=yes
	done
	pidof gamescope kazeta-guide mirrorrun mirrorrun-gl > /dev/null && playing=yes
	i=0
	[ -n "$playing" ] && echo "a game is running: not drawing the test pattern"
	for s in $card-*/status; do
		if [ -z "$playing" ] && [ "$(cat "$s")" = connected ]; then
			echo "test pattern on $(basename "$(dirname "$s")") for 10 s"
			t6-pattern /dev/dri/$(basename $card) $i 10 || true
		fi
		i=$((i + 1))
	done
fi

# HDMI audio: the adapter is its own ALSA card ("Trigger6"); it isn't a USB
# audio class card, so xbox-env keeps the game sound where it was.
grep -i trigger6 /proc/asound/cards
if [ "${TEST:-0}" = 1 ] && [ -z "$playing" ] && command -v aplay > /dev/null; then
	echo "2 s test tone on the adapter's HDMI"
	aplay -q -D hw:CARD=Trigger6,DEV=0 $K/tone-440.wav || true
fi

/etc/init.d/S99t6mirror start
sleep 6
echo "mirror:"
tail -5 /var/log/t6-mirror.log 2>/dev/null
pidof t6-mirror > /dev/null && echo "t6-mirror running" || echo "t6-mirror NOT running"
exit 0
