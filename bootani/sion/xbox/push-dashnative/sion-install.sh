#!/bin/sh
#
# Runs on the Sion as root. Puts the dashboard's 3D view back to the one the
# Xbox dashboard was made with (aspect 720/480 over its 640x480 picture, as on
# a real Xbox), by taking XBCOMPAT_DASH_VIEW=4:3 out of /etc/default/xbox.
# Nothing else changes: xbcompat and xboxdash.xbe stay as installed, and the
# 4:3 view can come back with REVERT=1.
#
# The dashboard reads the setting when it starts. NOW=1 restarts the Xbox
# session right away, but only when no game is running.
set -e
F=/etc/default/xbox

game_running() {
	for p in $(pidof xbcompat xbcompat-gpu); do
		tr '\0' ' ' < "/proc/$p/cmdline" | grep -q -e xboxdash -e Frolick -e Bootscreen || return 0
	done
	pidof gamescope kazeta-guide mirrorrun mirrorrun-gl > /dev/null && return 0
	return 1
}

touch $F
[ -e $F.pre-dashnative ] || cp -a $F $F.pre-dashnative
if [ "${REVERT:-0}" = 1 ]; then
	grep -q '^export XBCOMPAT_DASH_VIEW=4:3$' $F || echo 'export XBCOMPAT_DASH_VIEW=4:3' >> $F
	echo "dashboard view: 4:3 (stretched over the 720-wide picture)"
else
	sed -i '/XBCOMPAT_DASH_VIEW/d' $F
	echo "dashboard view: native (720/480, as on an Xbox)"
fi
sion persist $F > /dev/null
grep -n XBCOMPAT_DASH_VIEW $F || echo "$F: no XBCOMPAT_DASH_VIEW line"

if [ "${NOW:-0}" = 1 ]; then
	if game_running; then
		echo "a game is running: not restarting (the view changes at the next dashboard start)"
	else
		echo "restarting the Xbox session"
		sion restart xbox
	fi
else
	echo "the view changes at the next dashboard start (or run again with NOW=1)"
fi
exit 0
