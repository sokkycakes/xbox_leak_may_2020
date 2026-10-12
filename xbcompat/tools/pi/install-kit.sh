#!/bin/sh
# Runs on the Pi (see "Pi vblank.cmd"). Installs the xbcompat build whose
# Xbox vblank follows the display's real blanks, plus the full-speed CPU
# setting, after backing up what's there. "sh vblank.sh undo" puts the
# build that was installed before this one back ("undo original": the
# first backup, kept forever). If the dashboard doesn't come up, the build
# from before this one goes back on its own.
R=/tmp/vblank.txt
X=/opt/xbcompat
B=$X/backup-pre-vblank
P=$X/backup-previous
T=/tmp/push-vblank
LOGS="/var/log/xbox-session.log /var/log/xbox.log"
mount -o remount,rw / 2>/dev/null
# Put the build in directory $1 back.
restore() {
	cp -p $1/xbcompat-native.x86 $X/bin/xbcompat-native.x86
	cp -p $1/libxbcompat_renderer.so.1 $X/native/lib/libxbcompat_renderer.so.1
	if [ -e $1/S02cpufreq ]; then cp -p $1/S02cpufreq /etc/init.d/S02cpufreq
	else rm -f /etc/init.d/S02cpufreq; fi
	sync
}
# Start the dashboard again, clearing the flag a run of failed starts
# leaves (it makes the next start a no-op).
restart() { rm -f /var/run/xbox/dash-failed; /etc/init.d/S98xbox restart >/dev/null 2>&1; }
# Running, or about to: a title launch first builds its symbol map
# (xbrun.py --map-only) with no emulator process.
running() { pidof xbcompat-native.x86 xbcompat.x86 xbcompat.arm >/dev/null || ps | grep -q '[x]brun.py'; }
if [ "$1" = undo ]; then
	D=$P; [ "$2" = original ] && D=$B
	[ -d $D ] || { echo "No backup at $D." | tee $R; exit 1; }
	restore $D; restart
	echo "Put the build from $D back." | tee $R
	exit 0
fi
{
echo "== $(date)"
echo "== installed before"
md5sum $X/bin/xbcompat-native.x86 $X/native/lib/libxbcompat_renderer.so.1 /etc/init.d/S02cpufreq 2>&1
} > $R
# Back up once, so a second run doesn't overwrite the original with ours.
if [ ! -d $B ]; then
	mkdir -p $B
	cp -p $X/bin/xbcompat-native.x86 $X/native/lib/libxbcompat_renderer.so.1 $B/
	[ -e /etc/init.d/S02cpufreq ] && cp -p /etc/init.d/S02cpufreq $B/
	echo "== backed up to $B" >> $R
else
	echo "== backup already at $B, kept" >> $R
fi
# And the build installed right now, for a rollback to it.
rm -rf $P.new && mkdir -p $P.new &&
	cp -p $X/bin/xbcompat-native.x86 $X/native/lib/libxbcompat_renderer.so.1 $P.new/ &&
	{ [ ! -e /etc/init.d/S02cpufreq ] || cp -p /etc/init.d/S02cpufreq $P.new/; } &&
	rm -rf $P && mv $P.new $P && echo "== current build saved to $P" >> $R
install -m 755 $T/xbcompat-native.x86 $X/bin/xbcompat-native.x86
install -m 755 $T/libxbcompat_renderer.so.1 $X/native/lib/libxbcompat_renderer.so.1
install -m 755 $T/S02cpufreq /etc/init.d/S02cpufreq
install -m 755 $T/clocks.sh /root/clocks.sh
install -m 755 $T/vblank.sh /root/vblank.sh
sync
/etc/init.d/S02cpufreq start
for l in $LOGS; do [ -f $l ] && wc -c < $l > /tmp/vblank.$(basename $l).off; done
echo "restarting the dashboard..." >&2
restart
sleep 25
{
echo "== installed now"
md5sum $X/bin/xbcompat-native.x86 $X/native/lib/libxbcompat_renderer.so.1
echo "== running: $(pidof xbcompat-native.x86 xbcompat.x86 xbcompat.arm)"
for l in $LOGS; do
	[ -f $l ] || continue
	o=$(cat /tmp/vblank.$(basename $l).off 2>/dev/null || echo 0)
	echo "== $l since restart"
	tail -c +$((o + 1)) $l | tail -n 60
done
} >> $R 2>&1
# A title launch (the dashboard execs the emulator again) leaves a moment
# with nothing running: look for 20 seconds before deciding it failed.
up=0
for i in 1 2 3 4 5 6 7 8 9 10; do running && { up=1; break; }; sleep 2; done
if [ $up = 0 ]; then
	echo "== the dashboard didn't start: putting the build from before back" >> $R
	restore $P; restart
	echo "The new build didn't start, so the earlier one is back." >&2
	exit 1
fi
echo "== clocks on the dashboard (10 s)" >> $R
sh /root/clocks.sh 10 >> $R 2>&1
grep -h "vblank:" $R | head -n 2 >&2
echo "Installed. The build from before is kept in $P (sh vblank.sh undo)." >&2
