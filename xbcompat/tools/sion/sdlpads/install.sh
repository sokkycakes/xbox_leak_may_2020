#!/bin/sh
# Run on the Sion from this folder. Swaps xbcompat's SDL2 for one built with
# libusb (tools/sion/patches/sdl2), so SDL's own Xbox 360 / Xbox One drivers
# drive wired pads and dongles like the 8BitDo Ultimate 2C themselves,
# player light included, instead of reading them through the kernel's xpad.
# Live and in /data/overlay; the old library is kept as
# libSDL2-2.0.so.0.pre-sdlpads, and uninstall.sh puts it back. Restarts the
# dashboard so the new library is in use straight away.
set -e
cd "$(dirname "$0")"
for r in "" /data/overlay; do
	L=$r/opt/xbcompat/lib
	# The overlay (copied over / at boot) may carry only part of
	# /opt/xbcompat; give it a lib/ so the new SDL2 survives a reboot.
	[ -d $r/opt/xbcompat ] || continue
	mkdir -p "$L"
	# No SDL2 here before (the overlay often has none): remember that, so
	# uninstall.sh removes ours instead of restoring a backup.
	if [ ! -f $L/libSDL2-2.0.so.0.pre-sdlpads ] && [ ! -f $L/.sdlpads-added ]; then
		if [ -f $L/libSDL2-2.0.so.0 ]; then
			cp $L/libSDL2-2.0.so.0 $L/libSDL2-2.0.so.0.pre-sdlpads
		else
			touch $L/.sdlpads-added
		fi
	fi
	for f in libSDL2-2.0.so.0 libusb-1.0.so.0; do
		cp $f $L/$f.new && chmod 755 $L/$f.new && mv $L/$f.new $L/$f
	done
done
md5sum /opt/xbcompat/lib/libSDL2-2.0.so.0 /opt/xbcompat/lib/libusb-1.0.so.0
/etc/init.d/S98xbox restart
