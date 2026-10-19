#!/bin/bash
# Build the leak's Xbox Live dashboard (private/online/xodash, "XOnlineDash":
# network settings, connection status and account creation) into
# xodash.xbe with the leak's VC7 compiler, linker and imagebld under Wine,
# and stage an xbcompat disc and hard disk for it.
#
#   tools/xodashbuild/build.sh [WORK]     (default WORK=/tmp/xodash)
#   tools/xodashbuild/run.sh   [WORK] [xbrun options...]
#
# Needs wine + wine32, as tools/dashbuild does.  XODashLib comes prebuilt in
# the leak (XODashLib/obj/i386) as a debug build, so both XODashLib and
# XODashMain are compiled here, retail.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-/tmp/xodash}
LEAK=$(cd "$HERE/../../.." && pwd)/"xbox_leak_may_2020/xbox trunk/xbox"
mkdir -p "$WORK/obj" "$WORK/inc"
ln -sfn "$LEAK" "$WORK/xb"
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$WORK/wineprefix}
W=$(echo "Z:$WORK" | tr / '\\')
XB="$W\\xb"

# xonlinep.h is xonline.x copied (private/genx/online/xon/inc/makefile.inc).
cmp -s "$WORK/xb/private/genx/online/xon/inc/xonline.x" "$WORK/inc/xonlinep.h" ||
    cp "$WORK/xb/private/genx/online/xon/inc/xonline.x" "$WORK/inc/xonlinep.h"

inc() { local r="$W\\inc" d; for d in "$@"; do r="$r;$XB\\$d"; done; echo "$r"; }
# Each half's own INCLUDES order: XODashLib's crt io.h must win over ntos's.
LIBINC=$(inc private\\online\\xodash\\xodashlib public\\xdk\\inc private\\inc private\\ntos\\inc private\\ntos\\xapi\\inc private\\online \
         private\\ntos\\net public\\sdk\\inc public\\sdk\\inc\\crt public\\ddk\\inc)
MAININC=$(inc private\\online\\xodash\\xodashlib private\\inc private\\inc\\crypto \
         private\\ntos\\inc private\\ntos\\xapi\\inc private\\online\\kerb private\\online\\util private\\online \
         private\\ntos\\net private\\online\\test\\common public\\xdk\\inc public\\sdk\\inc public\\sdk\\inc\\crt \
         public\\ddk\\inc)
# Microsoft only built this title checked, and a few of its ASSERTs do the
# work (seek to a glyph, a XIP block, make a directory).  A retail build
# drops them, so those are kept as plain statements.  fixups.py makes
# account creation wait for the server and D: the xodash folder.  Patched
# copies are rewritten only when they change so their objects stay cached.
mkdir -p "$WORK/patch"
patch_src() { # dir file
    local real=$(ls "$1" | grep -ix "$2")
    sed -E 's/\bASSERT\((SetFilePointer|CreateDirectory|XFreeSectionByHandle|VirtualFree|pThis->Load)\b/(void)(\1/' \
        "$1/$real" | python3 "$HERE/fixups.py" "$real" > "$WORK/patch/$real.new"
    cmp -s "$WORK/patch/$real.new" "$WORK/patch/$real" && rm -f "$WORK/patch/$real.new" ||
        mv "$WORK/patch/$real.new" "$WORK/patch/$real"
}
cc_dir() { # dir include-path file...
    local src=$1 INC=$2; shift 2
    for f in "$@"; do
        real=$(ls "$src" | grep -ix "$f")
        o="$WORK/obj/${real%.*}.obj"
        file=$real dep=$src/$real inc=$INC
        # A patched copy still finds its "quoted" headers in the source folder.
        [ -f "$WORK/patch/$real" ] && file="$W\\patch\\$real" dep=$WORK/patch/$real \
            inc="$(echo "Z:$src" | tr / '\\');$INC"
        [ -s "$o" ] && [ "$o" -nt "$dep" ] && continue
        echo "cc $real"
        WORK="$WORK" XB="$XB" CC_DIR="$src" CC_INC="$inc" \
            python3 "$HERE/../dashbuild/cc.py" "$file" -DXONLINE_BUILD_LIBX
    done
}
X="$WORK/xb/private/online/xodash"
for f in font.cpp xip.cpp fileutil.cpp translator.cpp; do patch_src "$X/XODashLib" $f; done
for f in act_online.cpp accountdata.cpp xodash.cpp; do patch_src "$X/XODashMain" $f; done
cc_dir "$X/XODashLib" "$LIBINC" utilities.cpp xbinput.cpp xbutil.cpp xbstopwatch.cpp primitivescene.cpp msgdialog.cpp \
    camera.cpp falloff.cpp tgl.cpp smartobject.cpp button.cpp listbox.cpp spinner.cpp texture.cpp texturecache.cpp \
    material.cpp materiallibrary.cpp xip.cpp fileutil.cpp file.cpp inifile.cpp text.cpp locale.cpp translator.cpp \
    stringobj.cpp font.cpp date.cpp netconfig.cpp audio.cpp audiopump.cpp cdda.cpp ntiosvc.cpp xcdplay.cpp \
    dsoundmanager.cpp disctype.cpp
cc_dir "$X/XODashMain" "$MAININC" xodash.cpp main.cpp scene.cpp cellwallscene.cpp scenenavigator.cpp dialog.cpp keypad.cpp keyboard.cpp \
         accountdata.cpp act_online.cpp nts_settings.cpp nts.cpp nts_status.cpp nts_configchoice.cpp nts_help.cpp \
         automationmsg.cpp socket.cpp

L="$XB\\public\\xdk\\lib"
cd "$WORK"
objs=$(ls obj/*.obj | sed 's|^obj/|obj\\|')
echo "link xodash.exe"
timeout 600 wine "$WORK/xb/public/mstools/vc70/link.exe" /nologo -subsystem:xbox -fixed:no -nodefaultlib \
  -map:xodash.map -IGNORE:4001,4037,4039,4044,4065,4070,4078,4087,4089,4198,4108,4088 -STACK:262144,4096 \
  -merge:.rdata=.text -out:xodash.exe $objs \
  "$L\\d3d8.lib" "$L\\d3dx8.lib" "$L\\xgraphics.lib" "$L\\dsound.lib" "$L\\xonline.lib" "$L\\xapilib.lib" \
  "$XB\\private\\lib\\aug01\\i386\\Xapilibp.lib" "$L\\libcmt.lib" "$L\\libcpmt.lib" "$L\\xboxkrnl.lib" | tr -d '\r'

D="$XB\\private\\online\\xodash\\Media\\Locale"
ins=""
for l in english:English japanese:Japanese german:German french:French spanish:Spanish italian:Italian; do
    ins="$ins /INSERTFILE:$D\\${l%%:*}.inf,${l##*:}Xlate,N"
done
echo "imagebld xodash.xbe"
timeout 300 wine "$WORK/xb/public/idw/imagebld.exe" /TESTID:0xFFFE0000 /TESTRATINGS:0xFFFFFFFF \
  /TESTREGION:0x7FFFFFFF "/TESTNAME:Xbox Live" /INITFLAGS:0x00000004 /LIMITMEM /VERSION:0001 \
  $ins /TESTMEDIATYPES:1 /NOLIBWARN /NOSETUPHD /IN:xodash.exe /OUT:xodash.xbe | tr -d '\r'

# D: is C:\xodash (fixups.py), laid out as copymedia.bat copies it to the
# kit, with the title where the retail dashboards launched it.
S="$WORK/xb/private/online/xodash"
R="$WORK/run"
Y="$R/hdd/partition2/xodash"
mkdir -p "$R/disc" "$Y/media" "$Y/audio" "$R"/hdd/partition{1,3,4,5}
cp "$S"/*.xtf "$Y/"
cp -ru "$S/audio/mainaudio" "$Y/audio/"
for d in Xbg Xbx Content; do cp -ru "$S/Media/$d" "$Y/media/"; done
cp "$S"/Media/*.csv "$Y/media/"
cp xodash.xbe "$Y/xonlinedash.xbe"
echo "built $WORK/xodash.xbe; run it with $HERE/run.sh $WORK"
