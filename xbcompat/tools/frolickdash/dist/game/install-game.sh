#!/bin/sh
# Installs the Frolick dashboard as a game on this Sion, E:\Games\Frolick,
# from the files copied to /tmp: default.xbe, default.xip, the sounds
# (launch.wav, woosh.wav, blades_*.wav, menu_music.wav) and the HLE map.
# The Xbox dashboard is not touched: it lists Frolick in Games and launches
# it like any installed title.  L+R+Back+Start (or a tap of the power
# button) returns to the dashboard.  Uninstalling is deleting the folder
# (sion-game.sh remove).
G=/data/xbox/hdd/partition1/Games/Frolick
D=/opt/xbcompat/dash
mkdir -p "$G/Fonts" || exit 1
mv /tmp/default.xbe "$G/default.xbe" && mv /tmp/default.xip "$G/default.xip" || exit 1
for f in launch.wav woosh.wav blades_*.wav menu_music.wav; do for w in /tmp/$f; do [ -f "$w" ] && mv "$w" "$G/"; done; done
# The engine's outline fonts and the overlay's TrueType font, beside the XBE
# like the dashboard has them beside itself.
for f in Xbox.xtf "XBox Book.xtf"; do [ -f "$G/$f" ] || cp "$D/$f" "$G/$f"; done
[ -f "$G/Fonts/tahoma.ttf" ] || cp "$D/Fonts/tahoma.ttf" "$G/Fonts/"
chmod -R u+rw,a+r "$G"
# The HLE map under its cache name saves the first launch's signature scan;
# xbrun makes one itself if the cache differs.
for c in /data/xbox/cache/xbcompat /opt/xbcompat/cache; do
  if [ -d "$c" ]; then
    for m in /tmp/*-d3d8+dsound+xapilib-*.map; do [ -f "$m" ] && mv "$m" "$c/"; done
    break
  fi
done
ls -la "$G"
echo "Frolick is installed as a game: Games > Frolick on the dashboard"
