#!/bin/sh
#
# Runs on the Sion (install.sh sends the kit, unpacked in /tmp/sion-480i):
#
#   /usr/libexec/sion-video              the video setting (480i, 480p, auto)
#   /usr/share/sion/crimson-480i.edid    the 2880x480i EDID it puts on the HDMI port
#   /usr/lib/xbox/xbox-env               runs sion-video apply before reading KMS_MODE
#   /opt/xbcompat/tools/wincart-run      gamescope gets 2880x480 from KMS_MODE=2880x480i
#
# All kept with sion persist; the replaced files stay as *.pre-480i. The
# display plugged in now is remembered as the HDMI-to-AV box (auto gives it
# 480p) unless it already carries the 15 kHz EDID. NOW=1 restarts the
# dashboard so the setting applies at once; otherwise at the next boot.
K=/tmp/sion-480i
set -e

playing=
for p in $(pidof xbcompat); do
	tr '\0' ' ' < "/proc/$p/cmdline" | grep -q -e xboxdash.xbe -e 'Frolick' -e 'Bootscreen' || playing=xbcompat
done
pidof gamescope kazeta-guide mirrorrun mirrorrun-gl > /dev/null && playing=yes
if [ "${NOW:-0}" = 1 ] && [ -n "$playing" ]; then
	echo "a game is running: not restarting anything (run again with NOW=0, or after the game)"
	exit 1
fi

keep() {   # keep() FILE: a backup once, then sion persist
	[ -e "$1" ] && [ ! -e "$1.pre-480i" ] && cp -a "$1" "$1.pre-480i"
	return 0
}

install -D -m 755 $K/sion-video /usr/libexec/sion-video
install -D -m 644 $K/crimson-480i.edid /usr/share/sion/crimson-480i.edid
sion persist /usr/libexec/sion-video /usr/share/sion/crimson-480i.edid > /dev/null

E=/usr/lib/xbox/xbox-env
if ! grep -q sion-video $E; then
	keep $E
	awk '!done && /\/etc\/default\/kms/ {
		print "# sion-video sets KMS_MODE first (once a boot): 2880x480i, 15 kHz for a VGA"
		print "# transcoder, or 720x480 for the HDMI-to-AV box."
		print "[ -x /usr/libexec/sion-video ] && /usr/libexec/sion-video apply >> /var/log/sion-video.log 2>&1"
		done = 1
	} { print }' $E > $E.new
	grep -q sion-video $E.new || { echo "no KMS_MODE line in $E: not hooked"; rm -f $E.new; exit 1; }
	cat $E.new > $E && rm -f $E.new
	sion persist $E > /dev/null
	echo "hooked sion-video into $E"
fi

W=/opt/xbcompat/tools/wincart-run
if [ -f $W ] && ! grep -q 'oh=${oh%i}' $W; then
	keep $W
	awk '{ print } /^oh=\$\{mode#\*x\}$/ && !done { print "oh=${oh%i}   # 2880x480i: the 15 kHz mode for a VGA transcoder (sion-video)"; done = 1 }' $W > $W.new
	grep -q 'oh=${oh%i}' $W.new && cat $W.new > $W
	rm -f $W.new
	sion persist $W > /dev/null
	echo "wincart-run takes WxHi modes"
fi

V=/usr/libexec/sion-video
mkdir -p /data/sion
if [ ! -e /data/sion/video.conf ]; then
	# first install: the display plugged in now, if it's not ours, is the HDMI-to-AV box
	$V remember-av || true
fi
[ -n "$VIDEO" ] && $V "$VIDEO"
$V status

if [ "${NOW:-0}" = 1 ]; then
	$V apply --force
	sion restart dashboard
fi
rm -rf $K
