#!/bin/sh
# Builds mirrorrun against a melonDS checkout with melonds/mirror-tap.patch applied.
# MELONDS=/path/to/melonDS (built with cmake -B build -DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF)
set -e
M=${MELONDS:-/opt/src/melonDS}
# the libusb link (--serve usb) is built in when libusb-1.0 is found
if pkg-config --exists libusb-1.0 2>/dev/null; then USB_CFLAGS=-DHAVE_LIBUSB; USB_LIBS=$(pkg-config --libs libusb-1.0); fi
# the Sion's pads (--pads 1) need SDL2: SDL=prefix of a static SDL2 (see ../kit/sion/build-sdl.sh), or pkg-config's
if [ -n "$SDL" ]; then SDL_CFLAGS="-DHAVE_SDL -I$SDL/include/SDL2"; SDL_LIBS="$SDL/lib/libSDL2.a"
elif pkg-config --exists sdl2 2>/dev/null; then SDL_CFLAGS="-DHAVE_SDL $(pkg-config --cflags sdl2)"; SDL_LIBS=$(pkg-config --libs sdl2); fi
cd "$(dirname "$0")"
g++ -O2 -std=gnu++17 -DARCHITECTURE_x86_64=1 -DJIT_ENABLED -I"$M/src" -I"$M/build/src" -o mirrorrun mirrorrun.cpp platform.cpp hostlink.cpp dspico_sim.cpp tvout.cpp soundout.cpp pads.cpp saves.cpp $USB_CFLAGS $SDL_CFLAGS \
    "$M/build/src/libcore.a" "$M/build/src/teakra/src/libteakra.a" \
    $(ls "$M"/build/src/*/lib*.a "$M"/build/src/*/*/lib*.a 2>/dev/null | grep -v teakra) \
    $SDL_LIBS -lz -lpthread -lrt $USB_LIBS -ldl
