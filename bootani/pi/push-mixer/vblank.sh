#!/bin/sh
# Runs on the Pi (see "Pi vblank.cmd"). Installs the xbcompat build whose
# Xbox vblank follows the display's real blanks, plus the full-speed CPU
# setting, after backing up what's there. "sh vblank.sh undo" puts the
# backup back. If the dashboard doesn't come up, the backup goes back on
# its own.
R=/tmp/vblank.txt
X=/opt/xbcompat
B=$X/backup-pre-vblank
T=/tmp/push-vblank
LOGS="/var/log/xbox-session.log /var/log/xbox.log"
mount -o remount,rw / 2>/dev/null
restore() {
	cp -p $B/xbcompat-native.x86 $X/bin/xbcompat-native.x86
	cp -p $B/libxbcompat_renderer.so.1 $X/native/lib/libxbcompat_renderer.so.1
	if [ -e $B/S02cpufreq ]; then cp -p $B/S02cpufreq /etc/init.d/S02cpufreq
	else rm -f /etc/init.d/S02cpufreq; fi
	sync
}
running() { pidof xbcompat-native.x86 xbcompat.x86 xbcompat.arm >/dev/null; }
if [ "$1" = undo ]; then
	[ -d $B ] || { echo "No backup at $B." | tee $R; exit 1; }
	restore; /etc/init.d/S98xbox restart >/dev/null 2>&1
	echo "Put the earlier build back." | tee $R
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
install -m 755 $T/xbcompat-native.x86 $X/bin/xbcompat-native.x86
install -m 755 $T/libxbcompat_renderer.so.1 $X/native/lib/libxbcompat_renderer.so.1
install -m 755 $T/S02cpufreq /etc/init.d/S02cpufreq
install -m 755 $T/clocks.sh /root/clocks.sh
install -m 755 $T/vblank.sh /root/vblank.sh
sync
/etc/init.d/S02cpufreq start
for l in $LOGS; do [ -f $l ] && wc -c < $l > /tmp/vblank.$(basename $l).off; done
echo "restarting the dashboard..." >&2
/etc/init.d/S98xbox restart >/dev/null 2>&1
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
if ! running; then
	echo "== the dashboard didn't start: putting the earlier build back" >> $R
	restore; /etc/init.d/S98xbox restart >/dev/null 2>&1
	echo "The new build didn't start, so the earlier one is back." >&2
	exit 1
fi
echo "== clocks on the dashboard (10 s)" >> $R
sh /root/clocks.sh 10 >> $R 2>&1
grep -h "vblank:" $R | head -n 2 >&2
echo "Installed. The earlier build is kept in $B." >&2
