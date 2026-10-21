#!/bin/sh
#
# Runs on the Sion (install.sh sends it with the kit, unpacked in /tmp/xbox-live-kit):
#
#   /data/xbox-live          the Live dashboard's xbcompat, its map, the Live
#                            server, and xodash/ (xonlinedash.xbe and media:
#                            the leak's beta, or a retail one already there,
#                            which is kept; the beta is also in xodash-beta)
#   /opt/xbcompat/dash       the dashboard with the XBOX LIVE item (the old
#                            files kept as *.pre-live) and xodash -> the above,
#                            so C:\xodash\xonlinedash.xbe is there
#   /etc/default/xbox        launch-title in front of the title launcher
#
# The dashboard restarts only when it is what's running (no game).
set -e
K=/tmp/xbox-live-kit
L=/data/xbox-live
D=/opt/xbcompat/dash

# The leak's beta Live dashboard goes in xodash-beta; it is xodash too
# unless a retail one (a different xonlinedash.xbe) is already there.
beta=$(md5sum < $K/xbox-live/xodash/xonlinedash.xbe)
retail=
[ -f $L/xodash/xonlinedash.xbe ] && [ "$(md5sum < $L/xodash/xonlinedash.xbe)" != "$beta" ] &&
	[ ! -L $L/xodash ] && retail=1
mkdir -p $L
rm -rf $L/xodash-beta.new
cp -r $K/xbox-live/xodash $L/xodash-beta.new
rm -rf $L/xodash-beta
mv $L/xodash-beta.new $L/xodash-beta
cp $K/xbox-live/xodash.hlemap $L/xodash.hlemap.beta
if [ -z "$retail" ]; then
	rm -rf $L/xodash
	cp -r $L/xodash-beta $L/xodash
	cp $K/xbox-live/xodash.hlemap $L/xodash.hlemap
else
	echo "kept the retail Live dashboard in $L/xodash (the beta is in $L/xodash-beta)"
fi
mkdir -p $L/bin $L/liveserver
cp $K/xbox-live/bin/xbcompat $L/bin/xbcompat.new && mv $L/bin/xbcompat.new $L/bin/xbcompat
cp $K/xbox-live/liveserver/*.py $L/liveserver/
cp $K/xbox-live/launch-title $L/
chmod 755 $L/bin/xbcompat $L/launch-title
# The earlier side-by-side install's files.
rm -rf $L/hdd/partition2 $L/live-dash

files=""
keep=""
for f in xboxdash.xbe xboxdash.map default.xip mainmenu5.xip; do
	[ -f $D/$f.pre-live ] || cp $D/$f $D/$f.pre-live
	cp $K/dash/$f $D/$f.new && chmod 644 $D/$f.new && mv $D/$f.new $D/$f
	files="$files $D/$f"
	keep="$keep $D/$f.pre-live"
done
# The Games screen's scenes, for a dashboard from before they were added.
for f in NewGames.xip Games_Title.xip; do
	[ -f $D/$f ] && continue
	cp $K/dash/$f $D/$f && chmod 644 $D/$f
	files="$files $D/$f"
done
ln -sfn $L/xodash $D/xodash

touch /etc/default/xbox
sed -i '/xbox-live\/launch-title/d' /etc/default/xbox
cat >> /etc/default/xbox <<'END'
[ "$XBCOMPAT_LAUNCHER" = /data/xbox-live/launch-title ] || export XBOX_LIVE_NEXT_LAUNCHER="$XBCOMPAT_LAUNCHER"; export XBCOMPAT_LAUNCHER=/data/xbox-live/launch-title  # xbox-live/launch-title
END
sion persist $files $keep $D/xodash /etc/default/xbox > /dev/null
rm -rf $K

md5sum $L/bin/xbcompat $L/xodash/xonlinedash.xbe $D/xboxdash.xbe $D/default.xip $D/mainmenu5.xip
if ps w | grep -v grep | grep xbcompat | grep -q 'xboxdash.xbe$'; then
	/etc/init.d/S98xbox restart > /dev/null
	echo "restarted the dashboard: XBOX LIVE is under SETTINGS on the main menu"
elif pidof xbcompat > /dev/null; then
	echo "a game is running: the XBOX LIVE item shows up the next time the dashboard starts"
else
	echo "the dashboard isn't running: /etc/init.d/S98xbox start brings it up with XBOX LIVE"
fi
