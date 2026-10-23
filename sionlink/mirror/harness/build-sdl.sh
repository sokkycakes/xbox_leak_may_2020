#!/bin/sh
# A static SDL2 for mirrorrun's --pads: game controllers only (no video or audio), its
# HIDAPI drivers over libusb linked in, with xbcompat's patch that lets libusb reach
# Xbox-style pads (the same SDL as xbcompat's on the Sion, 2.32.10).
#   sh build-sdl.sh WORKDIR LIBUSB_PREFIX     then: SDL=WORKDIR/inst sh build.sh
set -e
W=${1:?work dir}; U=${2:?libusb prefix (a static libusb-1.0)}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W" && cd "$W"
[ -d SDL2-2.32.10 ] || git clone -q --depth 1 -b release-2.32.10 https://github.com/libsdl-org/SDL SDL2-2.32.10
cd SDL2-2.32.10
patch -p1 -N -s < "$HERE/patches/sdl2-hidapi-libusb-xbox-pads.patch" || true
mkdir -p b64 && cd b64
PKG_CONFIG_PATH=$U/lib/pkgconfig ../configure -q --prefix="$W/inst" --enable-static --disable-shared \
  --disable-video --disable-render --disable-audio --disable-sensor --disable-power --disable-filesystem \
  --disable-locale --disable-misc --disable-alsa --disable-pulseaudio --disable-pipewire --disable-jack \
  --disable-sndio --disable-libudev --disable-dbus --disable-ime --disable-ibus --disable-fcitx \
  --disable-loadso --enable-hidapi --enable-hidapi-libusb --enable-joystick
# libusb linked in, not loaded at run time (mirrorrun is static)
sed -i 's|^#define SDL_LIBUSB_DYNAMIC.*|/* libusb linked in */|' include/SDL_config.h
make -s -j"$(nproc)"
make -s install
