#!/bin/sh
# Full simulation: picture -> HDMI 2880x480i TMDS -> RTL -> composite -> software TV.
# Needs verilator, python3 with numpy + pillow. Usage: sim/run.sh picture.png [workdir]
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
pic=$1; w=${2:-/tmp/avsim}
mkdir -p "$w"
python3 "$here/sim/gen_hdmi.py" "$pic" "$w/in.tmds"
verilator --cc --exe --build -O3 -Wno-fatal -Wno-WIDTH "$here"/rtl/*.v "$here/sim/tb.cpp" \
    --top-module av_top -Mdir "$w/obj" -j 4 > "$w/build.log"
"$w/obj/Vav_top" "$w/in.tmds" "$w/out.bin" 0
python3 "$here/sim/check_rx.py" "$w/out.bin" "$w/in.tmds.src.png"
python3 "$here/sim/decode_ntsc.py" "$w/out.bin" "$w/composite.png" comp "$w/in.tmds.src.png"
python3 "$here/sim/decode_ntsc.py" "$w/out.bin" "$w/svideo.png" svideo "$w/in.tmds.src.png"
