#!/bin/bash
# Build the devkit dashboard (src/devkitdash.cpp) into devkitdash.xbe with the
# leak's VC7 compiler, linker and imagebld under Wine, the way
# tools/dashbuild does, and stage its files:
#
#   tools/devkitdash/build.sh [WORK]     (default WORK=/tmp/devkitdash)
#
#   WORK/devkit/            what goes to C:\devkit on the Sion
#       devkitdash.xbe
#       media/background.bmp  (BG=file.png to use another picture)
#       media/font.ttf, media/fontb.ttf  (DejaVu Sans unless FONT=/FONTB= say otherwise)
#
# Needs wine + wine32 (apt: libgd3:i386 first, then wine wine32:i386) and
# python3 with PIL for the background.
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-/tmp/devkitdash}
LEAK=$(cd "$HERE/../../.." && pwd)/"xbox_leak_may_2020/xbox trunk/xbox"
mkdir -p "$WORK/obj" "$WORK/devkit/media"
ln -sfn "$LEAK" "$WORK/xb"
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$WORK/wineprefix}
W=$(echo "Z:$WORK" | tr / '\\')
XB="$W\\xb"

src="$HERE/src/devkitdash.cpp"
if [ ! -s "$WORK/obj/devkitdash.obj" ] || [ "$src" -nt "$WORK/obj/devkitdash.obj" ]; then
    echo "cc devkitdash.cpp"
    WORK="$WORK" XB="$XB" CC_DIR="$HERE/src" CC_NO_UNICODE=1 \
        CC_INC="$XB\\public\\xdk\\inc" CC_OUT="$W\\obj\\" \
        python3 "$HERE/../dashbuild/cc.py" "$(echo "Z:$src" | tr / '\\')"
fi

L="$XB\\public\\xdk\\lib"; S="$XB\\public\\sdk\\lib\\i386"
cd "$WORK"
echo "link devkitdash.exe"
timeout 300 wine "$WORK/xb/public/mstools/vc70/link.exe" /nologo -subsystem:xbox -fixed:no -nodefaultlib \
  -map:devkitdash.lmap -IGNORE:4001,4037,4039,4044,4065,4070,4078,4087,4089,4198,4108,4088 -STACK:65536,4096 \
  -out:devkitdash.exe 'obj\devkitdash.obj' \
  "$S\\xfont.lib" "$L\\d3d8.lib" "$L\\xgraphics.lib" "$L\\xapilib.lib" \
  "$L\\libcmt.lib" "$L\\libcp.lib" "$L\\xboxkrnl.lib" | tr -d '\r'
[ -s devkitdash.exe ] || { echo "link failed" >&2; exit 1; }

# Title ID FFFE0000: xbcompat treats it as the dashboard (no soft reset into
# itself, no Guide over it), as it does the stock one and Frolick's.
echo "imagebld devkitdash.xbe"
timeout 300 wine "$WORK/xb/public/idw/imagebld.exe" /TESTID:0xFFFE0000 /TESTRATINGS:0xFFFFFFFF \
  /TESTVERSION:0x00000001 /TESTREGION:0x7FFFFFFF "/TESTNAME:Devkit Dashboard" /INITFLAGS:0x00000004 \
  /TESTMEDIATYPES:0xFFFFFFFF /NOLIBWARN /NOSETUPHD /INITFLAGS:0x00000000 \
  /IN:devkitdash.exe /OUT:devkitdash.xbe | tr -d '\r'
[ -s devkitdash.xbe ] || { echo "imagebld failed" >&2; exit 1; }
cp devkitdash.xbe "$WORK/devkit/"

# The background, as a 640x480 24-bit BMP the dashboard reads.
python3 "$HERE/tools/mkbg.py" "${BG:-$HERE/art/dashbg.png}" "$WORK/devkit/media/background.bmp"
FONT=${FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}
FONTB=${FONTB:-/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf}
cp "$FONT" "$WORK/devkit/media/font.ttf"
cp "$FONTB" "$WORK/devkit/media/fontb.ttf"
echo "built $WORK/devkit; run it with $HERE/run.sh $WORK"
