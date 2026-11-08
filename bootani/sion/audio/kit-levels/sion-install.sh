#!/bin/sh
#
# Runs on the Sion, from the folder it was copied to. App volume levels kit
# (push-levels): every app plays at the Guide's loudness.
#
#   sion-audiod -> /usr/bin (old one kept as sion-audiod.pre-levels): the
#                  router gives each app ONE fixed volume, measured over the
#                  app's first two minutes of sound and saved in
#                  /data/sion-audio/levels
#   guide/*.wav -> every Guide sound folder (old ones kept as *.pre-levels):
#                  12 dB quieter on disk, because the Guide inside an Xbox
#                  game gets that game's volume (+12 dB for most games)
#
# Refuses while a game is running (the router restarts, a 2 s sound gap).
set -e
K=$(cd "$(dirname "$0")" && pwd)

game_running() {
	for p in $(pidof xbcompat xbcompat-gpu); do
		tr '\0' ' ' < "/proc/$p/cmdline" | grep -q -e xboxdash -e Frolick -e Bootscreen || return 0
	done
	pidof gamescope kazeta-guide mirrorrun mirrorrun-gl > /dev/null && return 0
	return 1
}
if game_running; then
	echo "a game is running: nothing installed. Run this again once it is closed."
	exit 1
fi
(cd "$K" && md5sum -c MD5SUMS) > /dev/null || { echo "kit files damaged (MD5SUMS)"; exit 1; }

B=/usr/bin/sion-audiod
[ -e $B ] && [ ! -e $B.pre-levels ] && cp -a $B $B.pre-levels
cp $K/sion-audiod $B.new && chmod 755 $B.new && mv $B.new $B
persist="$B $B.pre-levels"

# The Guide's sound folders: xbcompat's (next to the binary) and any other
# copy (kazeta-guide). Only files still as shipped are swapped.
for d in $(find /opt /usr -name HUD_open.wav 2>/dev/null | xargs -r -n1 dirname | sort -u); do
	n=0
	while read -r sum f; do
		[ -e "$d/$f" ] || continue
		have=$(md5sum < "$d/$f" | cut -d' ' -f1)
		new=$(md5sum < "$K/guide/$f" | cut -d' ' -f1)
		[ "$have" = "$new" ] && continue
		if [ "$have" = "$sum" ]; then
			[ -e "$d/$f.pre-levels" ] || cp -a "$d/$f" "$d/$f.pre-levels"
			cp "$K/guide/$f" "$d/$f"
			persist="$persist $d/$f $d/$f.pre-levels"
			n=$((n + 1))
		else
			echo "$d/$f is not the shipped file: left as it is"
		fi
	done < "$K/guide/ORIGINAL.md5"
	echo "Guide sounds in $d: $n turned down 12 dB"
done
mkdir -p /data/sion-audio
sion persist $persist > /dev/null

# The wrapper in S07sion-audiod starts the new router within 2 s.
if pidof sion-audiod > /dev/null; then
	killall sion-audiod
else
	/etc/init.d/S07sion-audiod start
fi
sleep 4
pidof sion-audiod > /dev/null && echo "sion-audiod running" || echo "sion-audiod NOT running"
if [ -e /proc/asound/Loopback/pcm0p/sub0/status ]; then
	echo "app detection: ok"
else
	echo "app detection: no /proc/asound/Loopback/pcm0p/sub0/status (every app plays as before)"
fi
tail -6 /var/log/sion-audiod.log
cat /run/sion-audiod.status 2>/dev/null
md5sum $B
exit 0
