#!/bin/sh
#
# Installs Sion Link's DS pad on the Sion and brings the DS runtime up to date with it: a DS
# running Sion Link (sion-link-live.nds on its DSpico) is then an Xbox 360 pad for the
# dashboard, Frolick and games whenever no DS title runs, and a DS title takes the DS over
# for its mirror and hands it back. Run on the Sion, as root, from the kit folder:
#
#   sh install-pad.sh            installs dspad and its boot service, starts it
#
# /etc/init.d/S97sionlink-pad stop turns the pad off until the next boot.
#
# What goes where (kept across reboots, sion persist):
#   /opt/sionlink/dspad                 the pad service
#   /opt/sionlink/mirrorrun, -gl, dscart-run
#                                       the DS runtime, which asks dspad for the DS
#   /etc/init.d/S97sionlink-pad         starts it at boot; log /var/log/sionlink-pad.log
K=$(cd "$(dirname "$0")" && pwd)
B=/opt/sionlink
S=/etc/init.d/S97sionlink-pad

# nothing playing (as install-title.sh checks): a game would lose its pad or its DS
playing=
for p in $(pidof xbcompat); do
	tr '\0' ' ' < "/proc/$p/cmdline" | grep -q xboxdash.xbe || playing="a game"
done
pidof gamescope kazeta-guide > /dev/null && playing="a Windows game"
pidof mirrorrun mirrorrun-gl > /dev/null && playing="the DS mirror"
if [ -n "$playing" ]; then
	echo "$playing is running: not installing (go back to the dashboard first)"
	exit 1
fi

for f in dspad mirrorrun mirrorrun-gl dscart-run S97sionlink-pad; do
	[ -f "$K/$f" ] || { echo "the kit has no $f"; exit 1; }
done
[ -e /dev/uinput ] || echo "warning: no /dev/uinput, so the DS can't be a pad (the mirror still works)"

[ -x $S ] && $S stop
mkdir -p $B
for f in dspad mirrorrun mirrorrun-gl dscart-run; do
	cp "$K/$f" $B/$f.new && chmod 755 $B/$f.new && mv $B/$f.new $B/$f || { echo "couldn't copy $f"; exit 1; }
done
cp "$K/S97sionlink-pad" $S.new && chmod 755 $S.new && mv $S.new $S || { echo "couldn't copy the boot service"; exit 1; }
sion persist $B $S > /dev/null
$S start
sleep 2
if pidof dspad > /dev/null; then
	echo "the DS pad service is running"
else
	echo "the DS pad service is not running:"
	tail -n 3 /var/log/sionlink-pad.log
fi
md5sum $B/dspad $B/mirrorrun $B/mirrorrun-gl $B/dscart-run $S
