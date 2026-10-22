#!/bin/sh
# Sion Link live mirror, first hardware test (headless: the DS is the only screen, no sound).
#   sh run.sh /path/to/game.nds [GAMECODE] [60]
# Start sion-link-live.nds on the DS first and plug the DSpico's USB into the Sion.
# GAMECODE: AMKE for the Mario Kart DS kiosk demo; leave it out for other games.
# The DS shows 30 frames a second (the game still runs at 60); add 60 for every frame.
# Prints speed every 10 s. Ctrl+C stops it; when the DS is switched off it waits for it
# to come back (start Sion Link on the DS again). Saves are not kept.
cd "$(dirname "$0")"
chmod +x mirrorrun 2>/dev/null
ROM=$1
[ -n "$ROM" ] || { echo "usage: sh run.sh GAME.nds [GAMECODE] [60]"; exit 1; }
HALF="--half 1"
[ "$3" = 60 ] && HALF=
exec ./mirrorrun "$ROM" 0 --serve usb --reconnect 1 --realtime 1 --gxdelta 1 --jit 1 --threaded 1 $HALF ${2:+--gamecode $2}
