#!/bin/sh
# Record ROM in melonDS, replay the recording with the DS client in a second
# melonDS, compare every frame. usage: test.sh ROM FRAMES WORKDIR [INPUT]
# MIRROR_ARGS adds recorder options, e.g. "--half 1" for 30 fps.
set -e
H=$(cd "$(dirname "$0")" && pwd)
ROM=$1 FRAMES=$2 W=$3 IN=$4
: "${BLOCKSDS:=/opt/wonderful/thirdparty/blocksds/core}"; export BLOCKSDS PATH="$PATH:/opt/wonderful/bin"
mkdir -p "$W"
"$H/mirrorrun" "$ROM" "$FRAMES" --record "$W/mirror.bin" --stats "$W/sizes.csv" \
    --frames "$W/emu.raw" ${IN:+--input "$IN"} ${MIRROR_ARGS}
mkdir -p "$H/../ds/nitrofs"
cp "$W/mirror.bin" "$H/../ds/nitrofs/mirror.bin"
make -B -s -C "$H/../ds" >/dev/null 2>&1
cp "$H/../ds/mirrorplay.nds" "$W/client.nds"
"$H/mirrorrun" "$W/client.nds" $((FRAMES * 2 + 60)) --frames "$W/client.raw" --client-index "$W/client.idx"
python3 -I "$H/compare.py" "$W/emu.raw" "$W/client.raw" "$W/cmp" "$W/client.idx" "$W/mirror.bin"
python3 -I - "$W/sizes.csv" <<'PY'
import csv, sys
r = list(csv.DictReader(open(sys.argv[1])))[1:]
b = sorted(int(x["bytes"]) for x in r); z = sorted(int(x["zlib"]) for x in r)
gx = sum(int(x["gx"]) for x in r) / len(r)
print(f"bundle: avg {sum(b)/len(b)/1024:.2f} KB, median {b[len(b)//2]/1024:.2f}, p95 {b[int(len(b)*.95)]/1024:.2f}, max {b[-1]/1024:.1f}; zlib avg {sum(z)/len(z)/1024:.2f} KB; 3D part avg {gx/1024:.2f} KB")
PY
[ -n "$KEEP_RAW" ] || rm -f "$W/emu.raw" "$W/client.raw"
