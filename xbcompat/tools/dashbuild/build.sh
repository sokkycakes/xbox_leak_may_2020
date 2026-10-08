#!/bin/bash
# Build the leak's own Xbox dashboard (private/ui/xapp + private/ui/dash) into
# xboxdash.xbe with the leak's VC7 compiler, linker and imagebld under Wine,
# and stage an xbcompat hard disk for it.
#
#   tools/dashbuild/build.sh [WORK]     (default WORK=/tmp/xbdash)
#   tools/dashbuild/run.sh   [WORK] [xbrun options...]
#
# Needs wine + wine32 (apt: libgd3:i386 first, then wine wine32:i386).
# The leak path has spaces, which Wine command lines handle badly, so it is
# reached through the symlink WORK/xb.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-/tmp/xbdash}
LEAK=$(cd "$HERE/../../.." && pwd)/"xbox_leak_may_2020/xbox trunk/xbox"
mkdir -p "$WORK/obj" "$WORK/patch"
ln -sfn "$LEAK" "$WORK/xb"
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$WORK/wineprefix}
W=$(echo "Z:$WORK" | tr / '\\')          # WORK as a Wine path
XB="$W\\xb"

# GuidDef.h defines `one`, which main.cpp uses as a variable name.
sed 's/\bone\b/fOne/g' "$WORK/xb/private/ui/xapp/main.cpp" > "$WORK/patch/main.cpp"

for f in $(cat "$HERE/sources.txt"); do
    o="$WORK/obj/${f%.cpp}.obj"
    [ -s "$o" ] && continue
    src=$f; dir="$WORK/xb/private/ui/xapp"
    [ "$f" = main.cpp ] && src="$W\\patch\\main.cpp"
    echo "cc $f"
    WORK="$WORK" XB="$XB" CC_DIR="$dir" python3 "$HERE/cc.py" "$src" -D_AUDIO -D_CDPLAYER
done

L="$XB\\public\\xdk\\lib"; P="$XB\\private\\lib\\aug01\\i386"
cd "$WORK"
objs=$(ls obj/*.obj | sed 's|^obj/|obj\\|')
echo "link xboxdash.exe"
timeout 600 wine "$WORK/xb/public/mstools/vc70/link.exe" /nologo -subsystem:xbox -fixed:no -nodefaultlib \
  -map:xboxdash.map -IGNORE:4001,4037,4039,4044,4065,4070,4078,4087,4089,4198,4108,4088 -STACK:262144,4096 \
  -merge:.rdata=.text -out:xboxdash.exe $objs \
  "$XB\\private\\ui\\dvd\\dongle\\lib\\i386\\dvdlib.lib" "$XB\\private\\ui\\wmaenc\\wma_only_xbox.lib" \
  "$L\\d3d8.lib" "$L\\d3dx8.lib" "$L\\xgraphics.lib" "$L\\dsound.lib" "$L\\xapilib.lib" \
  "$P\\xapilibp.lib" "$P\\dashrecovery.lib" "$L\\libcmt.lib" "$L\\libcp.lib" "$L\\xboxkrnl.lib" | tr -d '\r'

D="$XB\\private\\ui\\dash"
ins=""
for l in english:English japanese:Japanese german:German french:French spanish:Spanish italian:Italian; do
    ins="$ins /INSERTFILE:$D\\${l%%:*}.txt,${l##*:}Xlate,N"
done
echo "imagebld xboxdash.xbe"
timeout 300 wine "$WORK/xb/public/idw/imagebld.exe" /TESTID:0xFFFE0000 /TESTRATINGS:0xFFFFFFFF \
  /TESTREGION:0x7FFFFFFF "/TESTNAME:Xbox Dashboard" /INITFLAGS:0x00000004 /LIMITMEM \
  "/INSERTFILE:$D\\obj\\xipsums.bin,XIPS,R" $ins /TESTMEDIATYPES:1 /NOLIBWARN /NOSETUPHD \
  /INITFLAGS:0x00000000 /IN:xboxdash.exe /OUT:xboxdash.xbe | tr -d '\r'

# The dashboard lives on partition 2 (its Y: drive) next to its XIPs and fonts.
R="$WORK/run"
mkdir -p "$R/disc" "$R"/hdd/partition{1,2,3,4,5}
cp -u "$WORK"/xb/private/ui/dash/*.xip "$WORK"/xb/private/ui/dash/*.xtf "$R/hdd/partition2/"
cp -ru "$WORK/xb/private/ui/dash/Audio" "$WORK/xb/private/ui/dash/Fonts" "$R/hdd/partition2/" 2>/dev/null || true
cp xboxdash.xbe "$R/hdd/partition2/"
echo "built $WORK/xboxdash.xbe; run it with $HERE/run.sh $WORK"
