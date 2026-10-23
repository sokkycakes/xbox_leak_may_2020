#!/bin/sh
# Play an Xbox disc image while it downloads (play as you download), as a
# test beside the installed system: nothing in /opt/xbcompat changes. Runs
# on the Sion from /data/frolick/stream with this kit's xbcompat (the
# installed combined build, push-liveguide ff23b324, plus streamed images:
# src/kernel/dvd.c) and frolick-store -stream fetching the image from the
# Archive by byte ranges.
#
#   sh /data/frolick/stream/stream-play.sh [NAME URL MD5 SHA1]
#   sh /data/frolick/stream/stream-play.sh --fetch-only [NAME URL MD5 SHA1]
#
# Defaults to Half-Life 2 from the xbox_essentials item. The image goes to
# /data/frolick/stream/NAME/disc.iso and is kept: a second run starts with
# what the first fetched. What happens:
#   1. the fetcher starts (that is "pressing Play")
#   2. the launch gate: the disc's directory and default.xbe come in first
#   3. the dashboard steps aside (xbox-av keeps the screen, no mode change)
#   4. the title starts from the streamed disc; the rest of the disc fills
#      in behind the reads it makes
#   5. when it quits, the dashboard comes back
# --fetch-only stops after the gate and lets the whole image come in and be
# checked, without touching the dashboard. Times and byte counts land in
# NAME/result.txt.
D=/data/frolick/stream
FETCH_ONLY=
[ "$1" = --fetch-only ] && { FETCH_ONLY=1; shift; }
NAME=${1:-hl2}
URL=${2:-https://archive.org/download/xbox_essentials/Half-Life%202.iso}
MD5=${3:-510f7b04eec3c50f6513d056b039097b}
SHA1=${4:-fae0a9a3cabff6a79ff082072e81114059559729}
G=$D/$NAME
SOCK=/tmp/frolick-stream.sock
mkdir -p "$G"

ms() { awk '{ printf "%d", $1 * 1000 }' /proc/uptime; }
stat_of() { sed -n "s/^$1 //p" "$G/stats" 2>/dev/null; }

# Never over a game or another test: the dashboard has to be the one thing
# on the screen (xbcompat running xboxdash.xbe, nothing else using it).
dash=
for p in $(pidof xbcompat); do
	if tr '\0' ' ' < /proc/$p/cmdline | grep -q 'xboxdash.xbe'; then
		dash=$p
	else
		echo "a game is running (xbcompat $p): not starting"; exit 1
	fi
done
if [ -z "$FETCH_ONLY" ]; then
	[ -n "$dash" ] || { echo "the dashboard isn't running (another test on the screen?): not starting"; exit 1; }
	if [ -s /var/run/wincart/gamescope.pid ] && [ -d /proc/$(cat /var/run/wincart/gamescope.pid) ]; then
		echo "a Windows game is running: not starting"; exit 1
	fi
	for other in mirrorrun melonDS gamescope theseus; do
		pidof $other > /dev/null && { echo "$other is running: not starting"; exit 1; }
	done
fi
pidof frolick-store-stream > /dev/null && { echo "a stream is already running"; exit 1; }

T0=$(ms)
rm -f "$G/default.xbe" "$G/stats"
# its own name, so it is never taken for the store helper
cp -f $D/frolick-store $D/frolick-store-stream
$D/frolick-store-stream -stream "$URL" -image "$G/disc.iso" -socket $SOCK \
	-xbe "$G/default.xbe" -stats "$G/stats" -md5 "$MD5" -sha1 "$SHA1" >> "$G/fetch.log" 2>&1 &
FETCH=$!

echo "waiting for the disc's directory and default.xbe"
while [ ! -s "$G/default.xbe" ]; do
	[ -d /proc/$FETCH ] || { echo "the fetcher stopped:"; tail -n 5 "$G/fetch.log"; exit 1; }
	sleep 0.1
done
T1=$(ms)
echo "gate open after $((T1 - T0)) ms, $(stat_of fetched) bytes fetched"

if [ -n "$FETCH_ONLY" ]; then
	echo "fetching the rest (progress in $G/stats)"
	while [ -z "$(stat_of verified)" ]; do
		[ -d /proc/$FETCH ] || { echo "the fetcher stopped:"; tail -n 5 "$G/fetch.log"; exit 1; }
		sleep 2
	done
	T4=$(ms)
	{
		echo "title $NAME ($URL), fetch only"
		echo "gate_ms $((T1 - T0))"
		echo "complete_ms $((T4 - T0))"
		cat "$G/stats"
	} > "$G/result.txt"
	cat "$G/result.txt"
	kill $FETCH 2>/dev/null
	exit 0
fi

# The dashboard steps aside: the session loop, then its xbcompat. xbox-av
# stays, holding the screen.
start-stop-daemon -K -q -p /var/run/xbox.pid
killall -q xbcompat
for _ in 1 2 3 4 5 6 7 8 9 10; do pidof xbcompat > /dev/null || break; sleep 0.3; done
killall -q -9 xbcompat

. /usr/lib/xbox/xbox-env
# shellcheck source=/dev/null
[ -r /etc/default/xbox ] && . /etc/default/xbox
export XBCOMPAT_BIN=$D/xbcompat
# The title and any it starts (XLaunchNewImage) go through xbrun.py with
# this xbcompat, not the Live launcher, which would pick /opt's.
export XBCOMPAT_LAUNCHER="$X/bin/python3 $X/tools/xbrun.py"
export XDG_CACHE_HOME=/data/xbox/cache
LOG=$G/xbcompat.log
T2=$(ms)
# shellcheck disable=SC2086
$X/bin/python3 $X/tools/xbrun.py "$G/default.xbe" --hdd /data/xbox/hdd --dvd "$G/disc.iso" \
	--d-path '\Device\CdRom0' --xbe-path default.xbe $XBOX_ARGS > "$LOG" 2>&1 &
GAME=$!

# The first frame, and what had come in by then.
T3= F3=
while [ -d /proc/$GAME ]; do
	if [ -z "$T3" ] && grep -q 'D3D: first Present' "$LOG"; then
		T3=$(ms); F3=$(stat_of fetched)
		echo "first frame after $((T3 - T0)) ms, $F3 bytes fetched"
	fi
	sleep 0.1
done
T4=$(ms)
{
	echo "title $NAME ($URL)"
	echo "gate_ms $((T1 - T0))"
	echo "title_started_ms $((T2 - T0))"
	echo "first_frame_ms $([ -n "$T3" ] && echo $((T3 - T0)) || echo none)"
	echo "fetched_at_first_frame ${F3:-none}"
	echo "played_ms $((T4 - T2))"
	cat "$G/stats"
} > "$G/result.txt"
cat "$G/result.txt"

# Back to the dashboard. The fetcher stops too; the next run picks up where
# this one left off.
kill $FETCH 2>/dev/null
/etc/init.d/S98xbox start
