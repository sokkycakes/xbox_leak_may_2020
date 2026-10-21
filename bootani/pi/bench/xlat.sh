#!/bin/sh
# Runs on the Pi (bench request 3). Installs xbcompat's own translator build
# (xbcompat.xlat: the whole emulator native ARM, the Xbox code translated by
# xlat, no box86) in the slot the launcher runs for XBOX_CPU=unicorn, and
# selects it. The Unicorn build is kept as xbcompat.arm.unicorn.
#   sh xlat.sh        install and start the dashboard on it
#   sh xlat.sh undo   the Unicorn build and the earlier XBOX_CPU back (box86)
# If the dashboard doesn't come up on xlat, undo runs on its own.
X=/opt/xbcompat/bin
T=/tmp/push-xlat
CONF=/etc/default/xbox
PREV=/opt/xbcompat/xlat-prev-cpu
R=/tmp/xlat.txt
LOGS="/var/log/xbox-session.log /var/log/xbox.log"
mount -o remount,rw / 2>/dev/null
restart() {
	/etc/init.d/S98xbox stop >/dev/null 2>&1
	killall xbcompat.arm xbcompat-native.x86 xbcompat.x86 2>/dev/null
	sleep 1
	rm -f /var/run/xbox/dash-failed
	/etc/init.d/S98xbox start >/dev/null 2>&1
}
running() { pidof xbcompat.arm >/dev/null || ps | grep -q '[x]brun.py'; }
# CONF without its XBOX_CPU lines, then $1 (written through, in case it's a link)
set_cpu() {
	grep -v '^[[:space:]]*\(export[[:space:]]*\)\{0,1\}XBOX_CPU=' $CONF > /tmp/xbox.conf.new
	[ -n "$1" ] && echo "$1" >> /tmp/xbox.conf.new
	cat /tmp/xbox.conf.new > $CONF
	rm -f /tmp/xbox.conf.new
}
undo() {
	[ -e $X/xbcompat.arm.unicorn ] && cp -p $X/xbcompat.arm.unicorn $X/xbcompat.arm
	set_cpu "$(cat $PREV 2>/dev/null)"
	rm -f $PREV
	sync
	restart
}
if [ "$1" = undo ]; then
	undo
	echo "Unicorn build and the earlier XBOX_CPU are back: $(grep XBOX_CPU $CONF || echo 'default (box86)')" | tee $R
	exit 0
fi
{
echo "== $(date)"
md5sum $X/xbcompat.arm $T/xbcompat.xlat $T/xbprof
} > $R
[ -e $X/xbcompat.arm.unicorn ] || cp -p $X/xbcompat.arm $X/xbcompat.arm.unicorn
[ -e $PREV ] || grep '^[[:space:]]*\(export[[:space:]]*\)\{0,1\}XBOX_CPU=' $CONF > $PREV
install -m 755 $T/xbcompat.xlat $X/xbcompat.arm
install -m 755 $T/xlat.sh /root/xlat.sh
install -m 755 $T/xbprof /root/xbprof
set_cpu XBOX_CPU=unicorn
sync
for l in $LOGS; do [ -f $l ] && wc -c < $l > /tmp/xlat.$(basename $l).off; done
echo "restarting the dashboard on xlat..." >&2
restart
sleep 25
up=0
for i in 1 2 3 4 5 6 7 8 9 10; do running && { up=1; break; }; sleep 2; done
{
echo "== installed now"
md5sum $X/xbcompat.arm
echo "== running: $(pidof xbcompat.arm)"
for l in $LOGS; do
	[ -f $l ] || continue
	o=$(cat /tmp/xlat.$(basename $l).off 2>/dev/null || echo 0)
	echo "== $l since restart"
	tail -c +$((o + 1)) $l | tail -n 60
done
} >> $R 2>&1
if [ $up = 0 ]; then
	echo "== the dashboard didn't start on xlat: box86 back" >> $R
	undo
	echo "xlat didn't start, so box86 is back (see $R)." >&2
	exit 1
fi
echo "Running on xlat. sh /root/xlat.sh undo puts box86 back." >&2
