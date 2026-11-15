#!/bin/sh
# Cross-builds the prototype with MinGW-w64 (apt install gcc-mingw-w64-x86-64).
set -e
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-x86_64-w64-mingw32-gcc}
FLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -static -s"
$CC $FLAGS -mwindows -o build/crimson-host.exe src/host.c -lws2_32 -lgdi32 -lmsimg32 -luser32
# GUI subsystem: no console window inside the partition (use --log for output)
$CC $FLAGS -mwindows -o build/crimson-agent.exe src/agent.c -lws2_32 -lgdi32 -luser32 -lwinmm -lm
ls -l build
