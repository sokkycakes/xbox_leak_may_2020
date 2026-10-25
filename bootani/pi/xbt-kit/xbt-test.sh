#!/bin/sh
# Runs on the Pi. Tries xbt, the 64-bit (AArch64) Xbox translator, next to
# the installed setup without changing it: everything lives in /data/xbt,
# the root filesystem and /opt/xbcompat are never touched, and the
# dashboard keeps running. "sh xbt-test.sh undo" deletes /data/xbt.
#
# xbt is one static binary with no libraries, so the image's 32-bit
# userland doesn't matter; it only needs the 64-bit kernel (kernel8.img).
# xbt has no graphics, sound or input yet, so this test is headless:
#   1. preflight: 64-bit kernel, page size, low-address mmap limit
#   2. the same i386 benchmarks timed under xbt and under the Pi's box86
#   3. the recovery dashboard (default.xbe) booted for 20 s, with stats
D=/data/xbt
R=/tmp/xbt-report.txt
REPS=${REPS:-5}
if [ "$1" = undo ]; then
	rm -rf $D /tmp/xbt-hdd
	echo "Removed $D. Nothing else was changed." | tee $R
	exit 0
fi
T=$(cd "$(dirname "$0")" && pwd)
if [ "$T" != $D ]; then
	rm -rf $D && mkdir -p $D && cp -a "$T"/. $D/ || { echo "cannot write $D" | tee $R; exit 1; }
fi
chmod 755 $D/xbt $D/bench/* $D/xbt-test.sh
# Hundredths of a second since boot (busybox date may lack %N).
now() { awk '{ printf "%d\n", $1 * 100 }' /proc/uptime; }
# Median of the numbers on stdin.
median() { sort -n | awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }'; }
# time_runs LABEL CMD...: runs CMD $REPS times, prints the median in
# hundredths and the program's result line.
time_runs() {
	label=$1; shift
	: > /tmp/xbt-times
	out=
	for i in $(seq $REPS); do
		s=$(now)
		out=$("$@" 2>&1 | tail -n 1)
		e=$(now)
		echo $((e - s)) >> /tmp/xbt-times
	done
	echo "$(median < /tmp/xbt-times) $out"
}
{
echo "== $(date)"
echo "== preflight"
echo "kernel: $(uname -r) $(uname -m)"
echo "page size: $(getconf PAGESIZE 2>/dev/null || echo ?)"
echo "vm.mmap_min_addr: $(cat /proc/sys/vm/mmap_min_addr) (xbt needs <= 65536)"
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) at $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) kHz"
echo "memory: $(awk '/MemAvailable/ { print $2 / 1024 " MB available" }' /proc/meminfo)"
echo "xbcompat running: $(pidof xbcompat-native.x86 xbcompat.x86 xbcompat.arm || echo no)"
echo "xbt md5: $(md5sum $D/xbt | cut -d' ' -f1)"
echo
echo "== xbt self-check (empty program)"
$D/xbt --elf $D/bench/b_null 2>&1 | tail -n 3
echo
echo "== benchmarks: median of $REPS runs, seconds (lower is better)"
echo "expected results: int20 0x5600de00, x87_20 and sse20 0xaf518800"
B86=$(command -v box86)
[ -n "$B86" ] || echo "box86 not found; xbt only"
for b in b_null int20 x87_20 sse20; do
	set -- $(time_runs xbt $D/xbt --elf $D/bench/$b)
	xt=$1; shift; xr="$*"
	if [ -n "$B86" ]; then
		set -- $(time_runs box86 env BOX86_LOG=0 BOX86_NOBANNER=1 $B86 $D/bench/$b)
		bt=$1; shift; br="$*"
	else bt=-; br=-; fi
	awk -v b=$b -v x=$xt -v y=$bt -v xr="$xr" -v yr="$br" 'BEGIN {
		r = (y != "-" && x > 0) ? sprintf("box86/xbt %.2fx", y / x) : ""
		printf "%-7s xbt %6.2f s [%s]   box86 %6s s [%s]   %s\n", b, x / 100, xr, (y == "-" ? "-" : sprintf("%.2f", y / 100)), yr, r }'
done
echo "(b_null is start-up only; the dashboard keeps running beside this, so"
echo " both translators see the same background load)"
echo
echo "== recovery dashboard, 20 s headless (no picture: xbt has no GPU yet)"
rm -rf /tmp/xbt-hdd && mkdir -p /tmp/xbt-hdd && cp $D/recovery/* /tmp/xbt-hdd/
$D/xbt --stats /tmp/xbt-hdd/default.xbe --run-seconds 20 --sample 2>&1 | tail -n 30
rm -rf /tmp/xbt-hdd
} > $R 2>&1
cat $R
