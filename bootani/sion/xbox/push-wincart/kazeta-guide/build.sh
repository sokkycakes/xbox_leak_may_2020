#!/bin/sh
# Builds kazeta-guide: xbcompat's Guide (xbcompat/src/guide/guide.cpp with
# guide-overlay-alpha.patch, on the frolickdash XUI runtime) plus host.cpp, as
# a 64-bit X11 client for gamescope inside Conty (glibc 2.41).
#
# Built on Ubuntu 24.04 (glibc 2.39): g++ 13, X11/GL/Xrender headers and
# libSDL2-2.0.so.0 (runtime only, no -dev package needed: the SDL headers come
# from the SDL 2.30.0 release tarball).
#
#   kazeta-guide/build.sh [OUT]       (from anywhere in a checkout)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../../../.." && pwd)
XB=$TOP/xbcompat
OUT=${1:-$HERE/kazeta-guide}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

curl -fsSL https://github.com/libsdl-org/SDL/releases/download/release-2.30.0/SDL2-2.30.0.tar.gz | tar -C "$W" -xzf - SDL2-2.30.0/include
mkdir -p "$W/lib" "$W/obj"
ln -s "$(ldconfig -p | sed -n 's/.*libSDL2-2.0.so.0 (libc6,x86-64) => //p' | head -n1)" "$W/lib/libSDL2.so"
cp "$XB/src/guide/guide.cpp" "$W/guide.cpp"
(cd "$W" && patch -p4 < "$HERE/guide-overlay-alpha.patch")

F="-O2 -g -fno-exceptions -fno-rtti -I$XB/tools/frolickdash/xui -I$W/SDL2-2.30.0/include -D_snprintf=snprintf -Wno-misleading-indentation"
for f in "$W/guide.cpp" "$XB"/tools/frolickdash/xui/xui_parse.cpp "$XB"/tools/frolickdash/xui/xui_scene.cpp \
         "$XB"/tools/frolickdash/xui/xui_draw.cpp "$XB"/tools/frolickdash/xui/xui_classes.cpp; do
	g++ $F -std=c++98 -fno-threadsafe-statics -c -o "$W/obj/$(basename "$f" .cpp).o" "$f"
done
g++ $F -std=c++11 -c -o "$W/obj/host.o" "$HERE/host.cpp"
g++ -o "$OUT" "$W"/obj/*.o -L"$W/lib" -lSDL2 -lGL -lX11 -lXrender -lm -ldl
strip "$OUT"
md5sum "$OUT"
