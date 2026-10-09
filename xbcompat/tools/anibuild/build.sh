#!/bin/bash
# Build the leak's original Xbox boot animation (private/ntos/ani2, its app/
# target: Bootscreen.xbe with a main()) with the leak's VC7 compiler, linker
# and imagebld under Wine, so it can run under xbcompat.
#
#   tools/anibuild/build.sh [WORK]      (default WORK=/tmp/xbani)
#   python3 tools/xbrun.py WORK/Bootscreen.xbe [xbcompat options...]
#
# app/sources links bootsnd_app.lib, the boot ROM's private copy of DirectSound,
# which the leak only has as a debug build. The release dsound.lib exposes the
# same API (DirectSoundCreate, SetEG, SetFilter, mix bins...), and xbcompat
# already replaces it, so the animation links against that instead.
# Needs wine + wine32, see tools/dashbuild/build.sh.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-/tmp/xbani}
LEAK=$(cd "$HERE/../../.." && pwd)/"xbox_leak_may_2020/xbox trunk/xbox"
mkdir -p "$WORK/obj" "$WORK/patch"
ln -sfn "$LEAK" "$WORK/xb"
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$WORK/wineprefix}
W=$(echo "Z:$WORK" | tr / '\\')
XB="$W\\xb"

ANI="$WORK/xb/private/ntos/ani2"
INC=$(for d in private\\ntos\\ani2 private\\atg\\samples\\common\\include public\\xdk\\inc private\\ntos\\inc \
      private\\inc public\\sdk\\inc; do printf '%s;' "$XB\\$d"; done)
SRCS="CamControl.cpp camera.cpp GreenFog.cpp logo_renderer.cpp qrand.cpp renderer.cpp scene_renderer.cpp
      Shield.cpp tex_gen.cpp VBlob.cpp xbs_app.cpp fastmath.cpp PlacementDoodad.cpp bootsound.cpp mslogo.cpp
      DEV.C CF.C EVF.C GLOBALS.C SOS.C PROC.C stboot.c xbinput.cpp"
# DEV.C was written against the Aug 2001 DirectSound mix-bin API.
python3 "$HERE/patch_dev.py" "$ANI/DEV.C" "$WORK/patch/DEV.C"
for f in $SRCS; do
    o="$WORK/obj/${f%.*}.obj"
    [ -s "$o" ] && continue
    echo "cc $f"
    src=$f; [ "$f" = DEV.C ] && src="$W\\patch\\DEV.C"
    # NOT_UNICODE=1 in app/sources: no -DUNICODE. FINAL_BUILD, as the boot
    # ROM's copy has it (defines.h): play once and return instead of looping,
    # and leave out the dev build's input and placement tools.
    WORK="$WORK" XB="$XB" CC_DIR="$ANI" CC_INC="$INC" CC_NO_UNICODE=1 \
        python3 "$HERE/../dashbuild/cc.py" "$src" -DBINARY_RESOURCE -DFINAL_BUILD
done

L="$XB\\public\\xdk\\lib"
cd "$WORK"
objs=$(ls obj/*.obj | sed 's|^obj/|obj\\|')
echo "link Bootscreen.exe"
timeout 600 wine "$WORK/xb/public/mstools/vc70/link.exe" /nologo -subsystem:xbox -fixed:no -nodefaultlib \
  -map:Bootscreen.map -IGNORE:4001,4037,4039,4044,4065,4070,4078,4087,4089,4198,4108,4088 -STACK:65536,4096 \
  -out:Bootscreen.exe $objs \
  "$L\\d3d8.lib" "$L\\xgraphics.lib" "$L\\dsound.lib" "$L\\xapilib.lib" \
  "$L\\libcmt.lib" "$L\\libcp.lib" "$L\\xboxkrnl.lib" | tr -d '\r'

echo "imagebld Bootscreen.xbe"
timeout 300 wine "$WORK/xb/public/idw/imagebld.exe" /TESTID:0x00112233 "/TESTNAME:Xbox Startup Sequence" \
  /INITFLAGS:0x00000000 /NOLIBWARN /IN:Bootscreen.exe /OUT:Bootscreen.xbe | tr -d '\r'
echo "built $WORK/Bootscreen.xbe"
