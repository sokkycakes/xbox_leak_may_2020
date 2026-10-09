#!/bin/bash
# Build the leak's own Xbox dashboard (private/ui/xapp + private/ui/dash) into
# xboxdash.xbe with the leak's VC7 compiler, linker and imagebld under Wine,
# and stage an xbcompat hard disk for it.
#
#   tools/dashbuild/build.sh [WORK]     (default WORK=/tmp/xbdash)
#   NEWGAMES=DIR tools/dashbuild/build.sh [WORK]
#
# NEWGAMES is the March 2001 dashboard's Games scene (TDATA\fffe0000\NewGames
# on that recovery disc), with its Games_Title scene beside it.  With it the
# Games screen is those scenes, packed into NewGames.xip and Games_Title.xip;
# without it the Games screen is built on Settings home.
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

# Patched sources, rewritten only when they change so their objects stay cached.
patch_src() { # file (as sources.txt spells it) sed-script
    sed "$2" "$WORK/xb/private/ui/xapp/$(ls "$WORK/xb/private/ui/xapp" | grep -ix "$1")" > "$WORK/patch/$1.new"
    cmp -s "$WORK/patch/$1.new" "$WORK/patch/$1" && rm -f "$WORK/patch/$1.new" || mv "$WORK/patch/$1.new" "$WORK/patch/$1"
}
# Apply a patch from games/ to xapp sources (LF line ends), as patch_src does.
patch_srcs() { # patch-file file...
    mkdir -p "$WORK/patch/new"
    for f in "${@:2}"; do tr -d '\r' < "$WORK/xb/private/ui/xapp/$f" > "$WORK/patch/new/$f"; done
    (cd "$WORK/patch/new" && patch -s -p1 < "$1") || exit 1
    for f in "${@:2}"; do
        cmp -s "$WORK/patch/new/$f" "$WORK/patch/$f" || mv "$WORK/patch/new/$f" "$WORK/patch/$f"
    done
    rm -rf "$WORK/patch/new"
}
# GuidDef.h defines `one`, which main.cpp uses as a variable name.
patch_src main.cpp 's/\bone\b/fOne/g'
# A game disc in the tray at startup waits in the Games screen's top slot
# instead of rebooting into the game (Microsoft's dashboard only ran with a
# game disc in when a game had sent it there).
patch_src Disc.cpp 's/theApp.m_bHasLaunchData || g_nDiscType == DISC_VIDEO/theApp.m_bHasLaunchData || g_nDiscType == DISC_TITLE || g_nDiscType == DISC_VIDEO/'
# The Memory screen lists each title's downloads (Xbox Live Arcade games) and
# the games installed from the Games screen next to its saves.  The patched
# TitleCollection.h sits beside the two patched sources, the only ones built
# with it (CTitleArray itself keeps its layout).
patch_srcs "$HERE/games/memory.patch" TitleCollection.h TitleCollection.cpp SavedGameGrid.cpp
# A source compiled from the patch folder is rebuilt when its header changes.
for f in TitleCollection.cpp SavedGameGrid.cpp; do
    [ "$WORK/patch/TitleCollection.h" -nt "$WORK/patch/$f" ] && touch "$WORK/patch/$f"
done

G=$(echo "Z:$HERE/games" | tr / '\\')    # the Games area added to the dashboard
for f in $(cat "$HERE/sources.txt") GameCollection.cpp; do
    o="$WORK/obj/${f%.cpp}.obj"
    src=$f; dir="$WORK/xb/private/ui/xapp"; dep=$dir/$f
    [ -f "$WORK/patch/$f" ] && src="$W\\patch\\$f" dep=$WORK/patch/$f
    [ "$f" = GameCollection.cpp ] && src="$G\\GameCollection.cpp" dep=$HERE/games/$f
    [ -s "$o" ] && [ "$o" -nt "$dep" ] && continue
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
XIPS=Games2.xip
if [ -n "$NEWGAMES" ]; then
    # An image as an XPR texture for the scene, the way mkxips.cmd's bundler step does.
    tga2xbx() { # tga width height
        printf 'Texture Tex\n{\n    Source %s\n    Format D3DFMT_A8R8G8B8\n    Width %s\n    Height %s\n    Levels 1\n}\n' "$1" "$2" "$3" > "${1%.tga}.rdf"
        timeout 120 wine "$WORK/xb/public/idw/bundler.exe" "${1%.tga}.rdf" -o "${1%.tga}.xbx" | tr -d '\r'
    }
    cp -r "$NEWGAMES" "$WORK/dash/NewGames"
    cp -r "$(dirname "$NEWGAMES")/Games_Title" "$WORK/dash/Games_Title"
    cp "$HERE/games/newgames.xap" "$WORK/dash/games.xap"
    (
        cd "$WORK/dash/NewGames"
        cp ../GameHilite_01.bmp .
        tga2xbx 1gamespanel.tga 512 512
        timeout 300 wine "$WORK/xb/private/ui/XIP/obj/i386/xip.exe" -q -m -i GameHilite_01.bmp ..\\NewGames.xip default.xap | tr -d '\r'
        cd "$WORK/dash/Games_Title"
        cp ../GameHilite_01.bmp .
        tga2xbx panel6.tga 512 256
        tga2xbx panel8.tga 512 512
        timeout 300 wine "$WORK/xb/private/ui/XIP/obj/i386/xip.exe" -q -m -i GameHilite_01.bmp ..\\Games_Title.xip default.xap | tr -d '\r'
    )
    XIPS="$XIPS NewGames.xip Games_Title.xip"
fi
(
    cd "$WORK/dash"
    E="python3 $HERE/games/xipedit.py"
    $E default.xip default.xip default.xap=default.xap games.xap=games.xap memory3.xap=memory3.xap
    $E mainmenu5.xip mainmenu5.xip default.xap=MainMenu5/default.xap
    $E settings3.xip Games2.xip default.xap=Games2/default.xap
    timeout 120 wine "$WORK/xb/private/ui/xipsign/obj/i386/xipsign.exe" obj\\xipsums.bin default.xip dvd.xip \
        Keyboard.xip JKeyboard.xip mainmenu5.xip Memory_Files2.xip Memory2.xip Message.xip music_copy3.xip \
        Music_PlayEdit2.xip music2.xip Settings_Clock.xip settings_language.xip settings_list.xip \
        settings_panel.xip settings_parental.xip settings_timezone.xip settings_video.xip settings3.xip \
        $XIPS > /dev/null
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
