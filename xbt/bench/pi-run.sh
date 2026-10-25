#!/bin/sh
# Time xbt and box86 on the same i386 benchmarks, on the target itself.
# Plain POSIX sh, so BusyBox ash works.
#
# usage: pi-run.sh [runs]   (from a directory holding xbt and b_null, b_int,
#                            b_x87, b_sse; box86 is taken from PATH if present)
cd "$(dirname "$0")" || exit 1
N=${1:-20}
export BOX86_LOG=0 BOX86_NOBANNER=1
BOX86=$(command -v box86)

now() { read -r up _ < /proc/uptime; echo "$up"; }      # 10 ms resolution
# average milliseconds per run of a command, run N times
run() {
    t0=$(now)
    i=0
    while [ "$i" -lt "$N" ]; do
        "$@" > /dev/null 2>&1
        i=$((i + 1))
    done
    t1=$(now)
    awk -v a="$t0" -v b="$t1" -v n="$N" 'BEGIN { printf "%.1f", (b - a) * 1000 / n }'
}

echo "kernel $(uname -m), $N runs each, box86: ${BOX86:-not found}"
printf "%-7s %10s %10s   %s\n" bench "xbt ms" "box86 ms" "same result"
for b in b_null b_int b_x87 b_sse; do
    rx=$(./xbt --elf "./$b" 2> /dev/null)
    tx=$(run ./xbt --elf "./$b")
    tb=-
    same=-
    if [ -n "$BOX86" ]; then
        rb=$("$BOX86" "./$b" 2> /dev/null)
        tb=$(run "$BOX86" "./$b")
        if [ "$rx" = "$rb" ]; then same=yes; else same="NO: xbt '$rx', box86 '$rb'"; fi
    fi
    printf "%-7s %10s %10s   %s\n" "$b" "$tx" "$tb" "$same"
done
echo "b_null is start-up alone; subtract it from the others to compare the translated code."
