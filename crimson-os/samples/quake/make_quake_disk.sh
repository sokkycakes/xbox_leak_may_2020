#!/bin/sh
# Builds quake-shareware.img: TyrQuake (64-bit, software renderer) plus the
# Quake 1.06 shareware episode, as a Game OS game disk.
#   samples/quake/make_quake_disk.sh [out.img]
# Needs: curl, unzip, lhasa (lha) or 7z, sfdisk, mtools.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-quake-shareware.img}
work=$(mktemp -d)
curl -sSfL -o "$work/quake106.zip" https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake/quake106.zip
(cd "$work" && unzip -q quake106.zip resource.1 && mkdir x && cd x && (lha xq ../resource.1 || 7z x -y ../resource.1 >/dev/null))
pak=$(find "$work/x" -iname pak0.pak)
echo "5906e5998fc3d896ddaf5e6a62e03abb  $pak" | md5sum -c --quiet
mkdir -p "$work/game/id1"
cp "$pak" "$work/game/id1/pak0.pak"
cp "$here/tyr-quake.exe" "$work/game/"
for f in SLICNSE.TXT LICINFO.TXT README.TXT; do cp "$(find "$work/x" -iname $f)" "$work/game/" 2>/dev/null || true; done
# -dibonly: plain GDI output (no DirectDraw in the Game OS). -window keeps the
# desktop mode, which is what the agent captures.
ARGS="-dibonly -window -width 640 -height 480 -nocdaudio" \
    "$here/../../tools/make_game_disk.sh" "$work/game" tyr-quake.exe "$out" 64
rm -rf "$work"
