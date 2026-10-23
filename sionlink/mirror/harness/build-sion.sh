#!/bin/sh
# The Sion kit's two builds (kit/sion): mirrorrun, all static, software 3D; and mirrorrun-gl,
# 64-bit with glibc linked (the Sion's Mesa libEGL is loaded at run time) and the rest static.
# SDL=prefix of the static SDL2 from build-sdl.sh, LIBUSB=prefix of a static libusb-1.0,
# MELONDS as for build.sh (build/ and build-gl/ both made).
set -e
M=${MELONDS:-/opt/src/melonDS}
: "${SDL:?SDL=prefix of a static SDL2}" "${LIBUSB:?LIBUSB=prefix of a static libusb-1.0}"
cd "$(dirname "$0")"
OUT=../kit/sion
SRC="mirrorrun.cpp platform.cpp hostlink.cpp dspico_sim.cpp tvout.cpp soundout.cpp pads.cpp saves.cpp menu.cpp"
FLAGS="-O2 -std=gnu++17 -DARCHITECTURE_x86_64=1 -DJIT_ENABLED -DHAVE_LIBUSB -DHAVE_SDL -I$SDL/include/SDL2 -I$LIBUSB/include -I$M/src"
libs() { echo "$M/$1/src/libcore.a $M/$1/src/teakra/src/libteakra.a $(ls "$M"/$1/src/*/lib*.a "$M"/$1/src/*/*/lib*.a 2>/dev/null | grep -v teakra)"; }

# shellcheck disable=SC2046,SC2086
g++ $FLAGS -I"$M/build/src" -o $OUT/mirrorrun $SRC $(libs build) \
    "$SDL/lib/libSDL2.a" "$LIBUSB/lib/libusb-1.0.a" -static -lz -lpthread -lrt -ldl
gcc -O2 -c -I"$M/src" "$M/src/frontend/glad/glad.c" -o /tmp/mirrorrun-glad.o
# shellcheck disable=SC2046,SC2086
g++ $FLAGS -I"$M/build-gl/src" -DHAVE_GL -DOGLRENDERER_ENABLED '-DMELONDS_GL_HEADER="frontend/glad/glad.h"' \
    -o $OUT/mirrorrun-gl $SRC glctx.cpp /tmp/mirrorrun-glad.o $(libs build-gl) \
    "$SDL/lib/libSDL2.a" "$LIBUSB/lib/libusb-1.0.a" "$(gcc -print-file-name=libz.a)" \
    -static-libstdc++ -static-libgcc -lpthread -lrt -ldl -lm
strip $OUT/mirrorrun $OUT/mirrorrun-gl
