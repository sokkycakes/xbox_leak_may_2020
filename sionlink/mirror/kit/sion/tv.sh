#!/bin/sh
# Sion Link live mirror with the game on the TV as well (run 11): both screens stacked on
# the TV in the mode it is already in (no resync), the game's sound on the TV's HDMI, and
# the same picture mirrored to the DS, whose pad plays it.
#   sh tv.sh /path/to/game.nds [GAMECODE] [60]    start (it carries on in the background)
#   sh tv.sh stop                                 stop; the dashboard comes back
# Start sion-link-live.nds on the DS and plug the DSpico's USB into the Sion; the game
# starts when the DS is there. The Xbox dashboard stops while the mirror has the TV.
# GAMECODE: AMKE for the Mario Kart DS kiosk demo. The DS shows 30 frames a second
# (the TV all 60); add 60 for every frame on the DS too. Saves are not kept.
# Log: /tmp/sionlink-tv.log
case $1 in /*|stop|'') ;; *) set -- "$PWD/$1" "$2" "$3" ;; esac   # the game's path from here
cd "$(dirname "$0")" || exit 1
SELF=$PWD/$(basename "$0")
LOG=/tmp/sionlink-tv.log
PIDFILE=/var/run/xbox.pid

if [ "$1" = stop ]; then
	pidof mirrorrun > /dev/null || { echo "the mirror isn't running"; exit 0; }
	killall mirrorrun
	for _ in $(seq 1 25); do
		pidof mirrorrun > /dev/null || break
		sleep 0.2
	done
	killall -q -9 mirrorrun
	echo "stopped; the dashboard is coming back"
	exit 0
fi

ROM=$1
[ -n "$ROM" ] || { echo "usage: sh tv.sh GAME.nds [GAMECODE] [60] | sh tv.sh stop"; exit 1; }
[ -r "$ROM" ] || { echo "no $ROM"; exit 1; }

if [ -z "$SIONLINK_TV_BG" ]; then
	pidof mirrorrun > /dev/null && { echo "the mirror is already running (sh tv.sh stop)"; exit 1; }
	# never over a game: the dashboard is xbcompat running xboxdash.xbe; a game it starts
	# replaces it in the same process, and Windows games run under gamescope
	playing=
	for p in $(pidof xbcompat); do
		tr '\0' ' ' < "/proc/$p/cmdline" | grep -q xboxdash.xbe || playing=xbcompat
	done
	pidof gamescope kazeta-guide > /dev/null && playing=wincart
	if [ -n "$playing" ]; then
		echo "a game is running ($playing): not taking the TV"
		exit 1
	fi
	# the rest runs on its own, so it outlives this shell and brings the dashboard back
	SIONLINK_TV_BG=1 setsid sh "$SELF" "$@" > "$LOG" 2>&1 < /dev/null &
	echo "started; log in $LOG"
	exit 0
fi

chmod +x mirrorrun 2>/dev/null
HALF="--half 1"
[ "$3" = 60 ] && HALF=

# the sound device and display socket the dashboard uses
eval "$(. /usr/lib/xbox/xbox-env > /dev/null 2>&1
	echo "AUDIODEV='$AUDIODEV' KMS_MODE='$KMS_MODE' XBOX_AV_SOCKET='$XBOX_AV_SOCKET'")"
export AUDIODEV KMS_MODE XBOX_AV_SOCKET
echo "$(date '+%F %T') sound device ${AUDIODEV:-default}, display socket ${XBOX_AV_SOCKET:-none}"

# The dashboard lets go of the TV and the sound: its session loop first, so it doesn't
# start the dashboard again, then xbcompat (as S98xbox stop does). xbox-av stays: it
# keeps the picture and hands the display over.
session=
if [ -r "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2> /dev/null; then
	session=1
	start-stop-daemon -K -q -p "$PIDFILE"
	rm -f "$PIDFILE"
fi
killall -q xbcompat
for _ in 1 2 3 4 5 6 7 8 9 10; do
	pidof xbcompat > /dev/null || break
	sleep 0.3
done
killall -q -9 xbcompat
echo "$(date '+%F %T') dashboard stopped${session:+ (session loop too)}"

# shellcheck disable=SC2086
./mirrorrun "$ROM" 0 --serve usb --reconnect 1 --realtime 1 --gxdelta 1 --jit 1 --threaded 1 \
	$HALF ${2:+--gamecode $2} --tv drm --sound alsa
echo "$(date '+%F %T') mirror exited with $?"

if [ -n "$session" ]; then
	start-stop-daemon -S -q -b -m -p "$PIDFILE" -x /bin/sh \
		-- -c "exec /usr/libexec/xbox-session >>/var/log/xbox-session.log 2>&1"
	echo "$(date '+%F %T') dashboard started again"
fi
