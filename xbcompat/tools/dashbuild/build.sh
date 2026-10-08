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

G=$(echo "Z:$HERE/games" | tr / '\\')    # the Games area added to the dashboard
for f in $(cat "$HERE/sources.txt") GameCollection.cpp; do
    o="$WORK/obj/${f%.cpp}.obj"
    [ -s "$o" ] && [ "$o" -nt "$HERE/games/$f" ] && continue
    src=$f; dir="$WORK/xb/private/ui/xapp"
    [ "$f" = main.cpp ] && src="$W\\patch\\main.cpp"
    [ "$f" = GameCollection.cpp ] && src="$G\\GameCollection.cpp"
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

# The dashboard's data with the Games area: patched scripts edited into the
# XIP archives, and a new signature table for them (the XBE's XIPS section).
rm -rf "$WORK/dash"
cp -r "$WORK/xb/private/ui/dash" "$WORK/dash"
python3 "$HERE/games/patch_dash.py" "$WORK/dash"
(
    cd "$WORK/dash"
    E="python3 $HERE/games/xipedit.py"
    $E default.xip default.xip default.xap=default.xap games.xap=games.xap
    $E mainmenu5.xip mainmenu5.xip default.xap=MainMenu5/default.xap
    $E music2.xip Games2.xip default.xap=Games2/default.xap
    timeout 120 wine "$WORK/xb/private/ui/xipsign/obj/i386/xipsign.exe" obj\\xipsums.bin default.xip dvd.xip \
        Keyboard.xip JKeyboard.xip mainmenu5.xip Memory_Files2.xip Memory2.xip Message.xip music_copy3.xip \
        Music_PlayEdit2.xip music2.xip Settings_Clock.xip settings_language.xip settings_list.xip \
        settings_panel.xip settings_parental.xip settings_timezone.xip settings_video.xip settings3.xip \
        Games2.xip > /dev/null
)

D="$W\\dash"
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
cp "$WORK"/dash/*.xip "$WORK"/dash/*.xtf "$R/hdd/partition2/"
cp -ru "$WORK/dash/Audio" "$WORK/dash/Fonts" "$R/hdd/partition2/" 2>/dev/null || true
mkdir -p "$R/hdd/partition1/Games"   # E:\Games: one folder per game, each with its default.xbe
cp xboxdash.xbe "$R/hdd/partition2/"
echo "built $WORK/xboxdash.xbe; run it with $HERE/run.sh $WORK"
