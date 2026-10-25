#!/bin/sh
# Rebuilds this kit from ../../../xbt on an x86-64 Linux machine with
# aarch64-linux-gnu-gcc, clang and lld. xbt is linked static-pie: no
# libraries, so it runs under the Pi image's 32-bit userland, and loaded
# high, because a plain -static binary sits at 0x400000, inside the low
# 4 GB that xbt reserves for the Xbox address space.
set -e
K=$(cd "$(dirname "$0")" && pwd)
X=$K/../../../xbt
make -C $X clean
make -C $X CFLAGS="-O2 -g -fPIE" LDFLAGS="-static-pie -lm"
aarch64-linux-gnu-strip -o $K/xbt $X/build/xbt
# The README's benchmarks, each run 20 times so one run is long enough to
# time on a Pi.
CC="clang --target=i386-linux-gnu"
CF="-O2 -march=pentium3 -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -nostdlib -static"
LF="-Wl,-Ttext=0x00401000 -Wl,--build-id=none -fuse-ld=lld"
O=$(mktemp -d)
mk() {
	out=$1 src=$2; shift 2
	$CC $CF "$@" -c -Dbench=bench_once -o $O/$out.o $X/bench/$src
	$CC $CF "$@" -DREPS=20 -c -o $O/$out.loop.o $K/loop.c
	$CC $CF "$@" -c -o $O/$out.rt.o $X/bench/rt.c
	$CC $CF $LF -o $K/bench/$out $O/$out.o $O/$out.loop.o $O/$out.rt.o
}
mkdir -p $K/bench $K/recovery
$CC $CF $LF -o $K/bench/b_null $X/bench/b_null.c $X/bench/rt.c
mk int20 b_int.c
mk x87_20 b_fp.c -mno-sse -DUSE_X87
mk sse20 b_fp.c -msse -mfpmath=sse
rm -rf $O
cp "$K/../../../xbox_leak_may_2020/xbox trunk/xbox/private/ntos/recovery/cdfiles/"* $K/recovery/
