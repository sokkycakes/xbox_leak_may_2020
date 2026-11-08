#!/bin/sh
#
# Runs on the Sion. Keeps the TV at 15 kHz 480i across Xbox session restarts.
# Frolick's "480p" display setting writes KMS_MODE=720x480. sion-video turned
# that into 2880x480i only once a boot, so a later `sion restart xbox` sent the
# TV 720x480p (31 kHz). sion-video apply now maps it on every session start.
# Old file kept as /usr/libexec/sion-video.pre-keep. NOW=1 restarts the Xbox
# session once (only when no game is running) to put the TV back on 480i.
set -e
K=$(cd "$(dirname "$0")" && pwd)
V=/usr/libexec/sion-video
OLD=ea8a1c2a2f7886766a2bf1821bcc76d4

game_running() {
	for p in $(pidof xbcompat xbcompat-gpu); do
		tr '\0' ' ' < "/proc/$p/cmdline" | grep -q -e xboxdash -e Frolick -e Bootscreen || return 0
	done
	pidof gamescope kazeta-guide mirrorrun mirrorrun-gl > /dev/null && return 0
	return 1
}

have=$(md5sum $V | cut -d' ' -f1)
if [ "$have" != "$OLD" ] && [ "$have" != "$(md5sum $K/sion-video | cut -d' ' -f1)" ]; then
	echo "$V isn't the version this kit expects ($have): not replaced"
	exit 1
fi
[ -e $V.pre-keep ] || cp -a $V $V.pre-keep
cp $K/sion-video $V
chmod 755 $V
sion persist $V > /dev/null
$V apply
echo "/etc/default/kms: $(cat /etc/default/kms)"
echo "/run/xbox-av-mode: $(cat /run/xbox-av-mode)"

if [ "${NOW:-0}" = 1 ]; then
	if game_running; then
		echo "a game is running: not restarting (the TV goes back to 480i at the next session start)"
	else
		echo "restarting the Xbox session so the TV goes back to 480i"
		sion restart xbox
	fi
fi
exit 0
