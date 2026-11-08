#!/bin/sh
#
# Runs on the Sion, from the folder it was copied to. Sound router kit:
#
#   snd-aloop.ko  -> /lib/modules/6.18.7/extra  (ALSA loopback card, loaded at boot)
#   sion-audiod   -> /usr/bin, S07sion-audiod -> /etc/init.d (started at boot
#                    through /etc/sion/late-start, before the Xbox session)
#   trigger6.ko   -> /lib/modules/6.18.7/extra  (JUA365 driver, rev d: turns
#                    audio on again after a mode set, as Windows does)
#   /usr/lib/xbox/xbox-env: while the router runs, AUDIODEV is the loopback
#                    (old file kept as xbox-env.pre-router)
#
# Games and the dashboard pick up the new AUDIODEV when the Xbox session
# starts. NOW=1 restarts it right away, but only when no game is running.
set -e
K=$(cd "$(dirname "$0")" && pwd)
D=/lib/modules/$(uname -r)

game_running() {
	for p in $(pidof xbcompat xbcompat-gpu); do
		tr '\0' ' ' < "/proc/$p/cmdline" | grep -q -e xboxdash -e Frolick -e Bootscreen || return 0
	done
	pidof gamescope kazeta-guide mirrorrun mirrorrun-gl > /dev/null && return 0
	return 1
}
keep() { [ -e "$1" ] && [ ! -e "$1.pre-router" ] && cp -a "$1" "$1.pre-router"; return 0; }

mkdir -p $D/extra
keep $D/modules.dep
cp $K/snd-aloop.ko $D/extra/snd-aloop.ko
grep -q '^extra/snd-aloop.ko:' $D/modules.dep || echo 'extra/snd-aloop.ko:' >> $D/modules.dep
C=/etc/modules-load.d/sion-extra.conf
mkdir -p /etc/modules-load.d
grep -qx snd-aloop $C 2>/dev/null || { echo '# ALSA loopback for the sound router (push-soundrouter)'; echo snd-aloop; } >> $C

# The router and the mirror hold the adapter: stop both to swap the driver.
[ -x /etc/init.d/S07sion-audiod ] && /etc/init.d/S07sion-audiod stop || true
[ -x /etc/init.d/S99t6mirror ] && /etc/init.d/S99t6mirror stop || true
killall sion-audiod t6-mirror 2>/dev/null && sleep 1 || true
cp $K/trigger6.ko $D/extra/trigger6.ko
cp $K/sion-audiod /usr/bin/sion-audiod
cp $K/S07sion-audiod /etc/init.d/S07sion-audiod
chmod 755 /usr/bin/sion-audiod /etc/init.d/S07sion-audiod
L=/etc/sion/late-start
mkdir -p /etc/sion
if ! grep -qx S07sion-audiod $L 2>/dev/null; then
	# first in the list, so it runs before anything that plays sound
	{ echo S07sion-audiod; cat $L 2>/dev/null; } > $L.new && mv $L.new $L
fi

E=/usr/lib/xbox/xbox-env
if ! grep -q 'sion-audiod' $E; then
	keep $E
	awk '!done && (/^# A USB sound card, when one is plugged in/ || /^# Same sound output bootani picks/) {
		print "# The sound router (sion-audiod, push-soundrouter), while it runs, takes all"
		print "# sound and sends it on to a USB sound card or the TV, plus a JUA365 adapter."
		print "if [ -z \"$AUDIODEV\" ] && [ -e /proc/asound/Loopback ] && pidof sion-audiod > /dev/null; then"
		print "\tAUDIODEV=plughw:CARD=Loopback,DEV=0"
		print "\texport AUDIODEV"
		print "fi"
		print ""
		done = 1
	} { print }' $E > $E.new
	if grep -q 'sion-audiod' $E.new; then
		cat $E.new > $E
		echo "xbox-env: sound goes through the router"
	else
		echo "no sound block found in $E: not hooked (sound unchanged)"
	fi
	rm -f $E.new
fi
sion persist $D/extra/snd-aloop.ko $D/extra/trigger6.ko $D/modules.dep $C /usr/bin/sion-audiod \
	/etc/init.d/S07sion-audiod $L $E > /dev/null

if grep -q '^trigger6 ' /proc/modules; then
	rmmod trigger6 || echo "could not unload the old trigger6; reboot to pick up the new one"
fi
modprobe trigger6 || insmod $D/extra/trigger6.ko || true
modprobe snd-aloop || insmod $D/extra/snd-aloop.ko
sleep 3
/etc/init.d/S07sion-audiod start
[ -x /etc/init.d/S99t6mirror ] && /etc/init.d/S99t6mirror start
sleep 2
cat /proc/asound/cards
tail -5 /var/log/sion-audiod.log
pidof sion-audiod > /dev/null && echo "sion-audiod running" || echo "sion-audiod NOT running"
( . $E; echo "AUDIODEV for new sessions: ${AUDIODEV:-unset}" )

if [ "${NOW:-0}" = 1 ]; then
	if game_running; then
		echo "a game is running: not restarting the Xbox session (the sound moves at the next start)"
	else
		echo "restarting the Xbox session so the sound goes through the router"
		sion restart xbox
	fi
fi
exit 0
