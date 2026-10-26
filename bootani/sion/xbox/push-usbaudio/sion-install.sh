#!/bin/sh
#
# Runs on the Sion, from the folder it was copied to with the three modules:
#
#   snd-rawmidi.ko snd-usbmidi-lib.ko snd-usb-audio.ko -> /lib/modules/6.18.7/extra
#       (USB sound cards; built from vanilla 6.18.7 with the Sion's fragments
#       plus CONFIG_SND_USB_AUDIO=m), loaded now and at boot by S11modules
#   /usr/lib/xbox/xbox-env      a USB sound card, when plugged in, takes AUDIODEV
#   /opt/xbcompat/tools/wincart-run   its dmix takes the USB card's hw device
#
# Replaced files stay as *.pre-usbaudio; all kept with sion persist. NOW=1
# restarts the dashboard so the sound moves at once (refused while a game runs).
set -e
K=$(cd "$(dirname "$0")" && pwd)
D=/lib/modules/$(uname -r)

playing=
for p in $(pidof xbcompat); do
	tr '\0' ' ' < "/proc/$p/cmdline" | grep -q -e xboxdash -e Frolick -e Bootscreen || playing=yes
done
pidof gamescope kazeta-guide mirrorrun mirrorrun-gl > /dev/null && playing=yes
if [ "${NOW:-0}" = 1 ] && [ -n "$playing" ]; then
	echo "a game is running: not restarting the dashboard"; exit 1
fi

keep() { [ -e "$1" ] && [ ! -e "$1.pre-usbaudio" ] && cp -a "$1" "$1.pre-usbaudio"; return 0; }

mkdir -p $D/extra
for m in snd-rawmidi snd-usbmidi-lib snd-usb-audio; do
	cp $K/$m.ko $D/extra/$m.ko
done
keep $D/modules.dep
grep -q '^extra/snd-rawmidi.ko:' $D/modules.dep || echo 'extra/snd-rawmidi.ko:' >> $D/modules.dep
grep -q '^extra/snd-usbmidi-lib.ko:' $D/modules.dep || echo 'extra/snd-usbmidi-lib.ko: extra/snd-rawmidi.ko' >> $D/modules.dep
grep -q '^extra/snd-usb-audio.ko:' $D/modules.dep || echo 'extra/snd-usb-audio.ko: extra/snd-usbmidi-lib.ko extra/snd-rawmidi.ko' >> $D/modules.dep
C=/etc/modules-load.d/sion-extra.conf
mkdir -p /etc/modules-load.d
grep -qx snd-usb-audio $C 2>/dev/null || { echo '# USB sound cards (push-usbaudio)'; echo snd-usb-audio; } >> $C
sion persist $D/extra/snd-rawmidi.ko $D/extra/snd-usbmidi-lib.ko $D/extra/snd-usb-audio.ko $D/modules.dep $C > /dev/null
modprobe snd-usb-audio
sleep 2
cat /proc/asound/cards

E=/usr/lib/xbox/xbox-env
if ! grep -q '/usbid' $E; then
	keep $E
	awk '!done && /^# Same sound output bootani picks/ {
		print "# A USB sound card, when one is plugged in, takes the sound: the Chromebox'"'"'s"
		print "# own headphone jack has no driver, and a VGA output carries none."
		print "if [ -z \"$AUDIODEV\" ]; then"
		print "\tfor c in /proc/asound/card[0-9]*; do"
		print "\t\tif [ -e \"$c/usbid\" ] && [ -e \"$c/pcm0p\" ]; then"
		print "\t\t\tAUDIODEV=plughw:CARD=$(cat \"$c/id\"),DEV=0"
		print "\t\t\texport AUDIODEV"
		print "\t\t\tbreak"
		print "\t\tfi"
		print "\tdone"
		print "fi"
		print ""
		done = 1
	} { print }' $E > $E.new
	grep -q '/usbid' $E.new || { echo "no sound block in $E: not hooked"; rm -f $E.new; exit 1; }
	cat $E.new > $E && rm -f $E.new
	sion persist $E > /dev/null
	echo "xbox-env: a USB sound card takes the sound"
fi

W=/opt/xbcompat/tools/wincart-run
if [ -f $W ] && ! grep -q 'plughw:CARD=\*,DEV=\*)' $W; then
	keep $W
	awk '{ print } /hdmi:CARD=PCH,DEV=2\) dev=hw:PCH,8 ;;/ && !done {
		print "plughw:CARD=*,DEV=*) c=${dev#plughw:CARD=}; dev=hw:${c%%,*},${dev##*DEV=} ;;   # a USB sound card (xbox-env)"
		done = 1 }' $W > $W.new
	grep -q 'plughw:CARD=\*,DEV=\*)' $W.new && cat $W.new > $W
	rm -f $W.new
	sion persist $W > /dev/null
	echo "wincart-run: plays to a USB sound card"
fi

( . $E; echo "AUDIODEV now: ${AUDIODEV:-unset}" )
[ "${NOW:-0}" = 1 ] && sion restart dashboard
exit 0
