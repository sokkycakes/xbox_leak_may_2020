#!/bin/sh
# Run on the Sion: puts back the SDL2 install.sh replaced and restarts the
# dashboard.
for r in "" /data/overlay; do
	L=$r/opt/xbcompat/lib
	[ -f $L/libSDL2-2.0.so.0.pre-sdlpads ] || continue
	mv $L/libSDL2-2.0.so.0.pre-sdlpads $L/libSDL2-2.0.so.0
	rm -f $L/libusb-1.0.so.0
done
md5sum /opt/xbcompat/lib/libSDL2-2.0.so.0
/etc/init.d/S98xbox restart
