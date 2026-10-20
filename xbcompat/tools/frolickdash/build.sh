#!/bin/bash
# Build the Frolick dashboard for the Xbox dashboard engine: the leak's
# xapp engine (built the way tools/dashbuild does, same objects) plus the
# nodes in nodes/ and the XUI runtime in xui/, linked into its own
# xboxdash.xbe with its own default.xip (the hub scene, the XUI panels, the
# font and the glyph art).  Nothing in the leak's dashboard is changed.
#
#   tools/frolickdash/build.sh [WORK]     (default WORK=/tmp/xbdash)
#
# WORK is shared with dashbuild: its first run compiles the engine there
# (about ten minutes under Wine); this script then adds its own objects and
# writes WORK/frolick.  Environment:
#   FONT    the TTF the panels use (default: DejaVu Sans from the system)
#   GLYPHS  a directory with Frolick's glyph_*.png (default: Xelu's Xbox 360
#           prompts in art/xelu360/, under Frolick's names)
#   ART     a directory with Frolick's Circle.png / label_*.png and pad
#           textures (default: tools/mkart.py's renderings)
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DASHBUILD="$HERE/../dashbuild"
WORK=${1:-/tmp/xbdash}
OUT="$WORK/frolick"

# The engine's objects, patched sources and Wine prefix come from dashbuild.
if [ ! -s "$WORK/obj/main.obj" ] || [ ! -d "$WORK/wineprefix" ]; then
    "$DASHBUILD/build.sh" "$WORK"
fi
LEAK=$(cd "$HERE/../../.." && pwd)/"xbox_leak_may_2020/xbox trunk/xbox"
ln -sfn "$LEAK" "$WORK/xb"
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$WORK/wineprefix}
W=$(echo "Z:$WORK" | tr / '\\')
XB="$W\\xb"
mkdir -p "$OUT/obj" "$OUT/dash"

# Our own sources: the XUI runtime and the new nodes.
for src in "$HERE"/xui/*.cpp "$HERE"/nodes/*.cpp; do
    f=$(basename "$src"); o="$OUT/obj/${f%.cpp}.obj"
    [ -s "$o" ] && [ "$o" -nt "$src" ] && [ "$o" -nt "$HERE/xui/xui.h" ] && continue
    echo "cc $f"
    WORK="$WORK" XB="$XB" CC_DIR="$WORK/xb/private/ui/xapp" CC_OUT="$(echo "Z:$OUT/obj/" | tr / '\\')" \
        python3 "$DASHBUILD/cc.py" "$(echo "Z:$src" | tr / '\\')" -D_AUDIO -D_CDPLAYER
done

# main.cpp, with the app's files found beside the XBE when it runs as a
# game (D:, from E:\Games\Frolick on a console) and on the dashboard
# partition (Y:) when it is the dashboard.  Built from dashbuild's patched
# copy, in place of the engine's main.obj.
mkdir -p "$OUT/patch"
python3 - "$WORK/patch/main.cpp" "$OUT/patch/main.cpp" <<'PY'
import sys
src, dst = sys.argv[1:3]
s = open(src, encoding="latin-1").read()
fn = r"""
// Frolick: its XIP, scenes and fonts sit beside the XBE when it runs as a
// game (D:), on the dashboard partition (Y:) when it is the dashboard.
static const TCHAR* AppDrive()
{
	static TCHAR szDrive [4];
	if (!szDrive[0])
		_tcscpy(szDrive, GetFileAttributes("d:\\default.xip") != 0xFFFFFFFF ? _T("d:\\") : _T("y:\\"));
	return szDrive;
}
"""
assert s.count('#include "xlaunch.h"\n') == 1
s = s.replace('#include "xlaunch.h"\n', '#include "xlaunch.h"\n' + fn)
reps = [
    ('LoadXIP(_T("y:\\\\default.xip"), true);',
     '{ TCHAR szXip [32]; _tcscpy(szXip, AppDrive()); _tcscat(szXip, _T("default.xip")); LoadXIP(szXip, true); }'),
    ('_tcscpy(szFileToLoad, _T("y:\\\\default.xap"));',
     '_tcscpy(szFileToLoad, AppDrive()); _tcscat(szFileToLoad, _T("default.xap"));'),
    ('_tcscpy(szAppDir, _T("y:\\\\"));', '_tcscpy(szAppDir, AppDrive());'),
    ('_tcscpy(g_szCurDir, _T("y:/default.xap"));',
     "_tcscpy(g_szCurDir, AppDrive()); g_szCurDir[2] = _T('/'); _tcscat(g_szCurDir, _T(\"default.xap\"));"),
]
for old, new in reps:
    assert s.count(old) == 1, old
    s = s.replace(old, new)
open(dst, "w", encoding="latin-1").write(s)
PY
if [ ! -s "$OUT/obj/main.obj" ] || [ "$OUT/patch/main.cpp" -nt "$OUT/obj/main.obj" ]; then
    echo "cc main.cpp"
    WORK="$WORK" XB="$XB" CC_DIR="$OUT/patch" CC_OUT="$(echo "Z:$OUT/obj/" | tr / '\\')" \
        python3 "$DASHBUILD/cc.py" "$(echo "Z:$OUT/patch/main.cpp" | tr / '\\')" -D_AUDIO -D_CDPLAYER
fi

# Link: the engine's objects (dashbuild's patched TitleCollection needs its
# GameCollection, so that comes along unused; main.obj is ours) plus our
# objects.
L="$XB\\public\\xdk\\lib"; P="$XB\\private\\lib\\aug01\\i386"
cd "$OUT"
objs=$(ls "$WORK"/obj/*.obj | grep -v '/main\.obj$' | sed "s|^$WORK/obj/|..\\\\obj\\\\|"; ls obj/*.obj | sed 's|^obj/|obj\\|')
echo "link xboxdash.exe"
timeout 600 wine "$WORK/xb/public/mstools/vc70/link.exe" /nologo -subsystem:xbox -fixed:no -nodefaultlib \
  -map:xboxdash.map -IGNORE:4001,4037,4039,4044,4065,4070,4078,4087,4089,4198,4108,4088 -STACK:262144,4096 \
  -merge:.rdata=.text -out:xboxdash.exe $objs \
  "$XB\\private\\ui\\dvd\\dongle\\lib\\i386\\dvdlib.lib" "$XB\\private\\ui\\wmaenc\\wma_only_xbox.lib" \
  "$L\\d3d8.lib" "$L\\d3dx8.lib" "$L\\xgraphics.lib" "$L\\dsound.lib" "$L\\xapilib.lib" \
  "$P\\xapilibp.lib" "$P\\dashrecovery.lib" "$L\\libcmt.lib" "$L\\libcp.lib" "$L\\xboxkrnl.lib" | tr -d '\r'

# The data: the generated hub and panels, the glyph art and the font.
rm -rf "$OUT/dash"; mkdir -p "$OUT/dash/pads" "$OUT/dash/obj"
cp "$HERE"/dash/pads/*.xui "$OUT/dash/pads/"
cp "$HERE"/art/blades/*.png "$OUT/dash/pads/"     # the Blades skin's button glyphs
python3 "$HERE/tools/genhub.py" "$OUT/dash"
python3 "$HERE/tools/genpage.py" "$OUT/dash"
python3 "$HERE/tools/mkart.py" "$OUT/dash"
python3 "$HERE/tools/mkbelt.py" "$HERE/art/gametap/beta2scene-belt.json" "$OUT/dash/belt.bin"
[ -n "$ART" ] && cp "$ART"/*.png "$OUT/dash/"
if [ -n "$GLYPHS" ]; then cp "$GLYPHS"/glyph_*.png "$OUT/dash/pads/"; else
    # Frolick's glyph files are Xelu's "Others/Xbox 360" prompts: glyph_a.png
    # is 360_Start (the game tile and the START strip), glyph_a_button.png
    # is 360_A (the carousel pill and the hub prompt), glyph_b/x/y are B/X/Y.
    X="$HERE/art/xelu360"
    cp "$X/360_Start.png" "$OUT/dash/pads/glyph_a.png"
    cp "$X/360_A.png" "$OUT/dash/pads/glyph_a_button.png"
    for b in b x y; do cp "$X/360_${b^^}.png" "$OUT/dash/pads/glyph_$b.png"; done
fi
cp "${FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}" "$OUT/dash/font.ttf"
(
    cd "$OUT/dash"
    python3 "$HERE/tools/mkassets.py" default.xip "$OUT/dash"
    timeout 120 wine "$WORK/xb/private/ui/xipsign/obj/i386/xipsign.exe" obj\\xipsums.bin default.xip > /dev/null
)

# The XBE, with the signatures of our XIP and the dashboard's language tables.
D="$W\\frolick\\dash"
ins=""
for l in english:English japanese:Japanese german:German french:French spanish:Spanish italian:Italian; do
    ins="$ins /INSERTFILE:$W\\dash\\${l%%:*}.txt,${l##*:}Xlate,N"
done
echo "imagebld xboxdash.xbe"
timeout 300 wine "$WORK/xb/public/idw/imagebld.exe" /TESTID:0xFFFE0000 /TESTRATINGS:0xFFFFFFFF \
  /TESTREGION:0x7FFFFFFF "/TESTNAME:Frolick" /INITFLAGS:0x00000004 /LIMITMEM \
  "/INSERTFILE:$D\\obj\\xipsums.bin,XIPS,R" $ins /TESTMEDIATYPES:1 /NOLIBWARN /NOSETUPHD \
  /INITFLAGS:0x00000000 /IN:xboxdash.exe /OUT:xboxdash.xbe | tr -d '\r'

# The same image as a game, E:\Games\Frolick\default.xbe, with its own
# title ID so the dashboard's Memory and Games screens list it apart from
# the dashboard ("FRLK").
echo "imagebld default.xbe (game)"
mkdir -p "$OUT/game"
timeout 300 wine "$WORK/xb/public/idw/imagebld.exe" /TESTID:0x46524C4B /TESTRATINGS:0xFFFFFFFF \
  /TESTREGION:0x7FFFFFFF "/TESTNAME:Frolick" /INITFLAGS:0x00000004 /LIMITMEM \
  "/INSERTFILE:$D\\obj\\xipsums.bin,XIPS,R" $ins /TESTMEDIATYPES:1 /NOLIBWARN /NOSETUPHD \
  /INITFLAGS:0x00000000 /IN:xboxdash.exe /OUT:game\\default.xbe | tr -d '\r'

# Stage partition 2 (Y:): our XIP, the engine's outline fonts, the XBE.
R="$OUT/run"
mkdir -p "$R/disc" "$R"/hdd/partition{1,2,3,4,5}
cp "$OUT/dash/default.xip" "$R/hdd/partition2/"
cp "$WORK"/dash/*.xtf "$R/hdd/partition2/"
cp -ru "$WORK/dash/Fonts" "$R/hdd/partition2/" 2>/dev/null || true
cp xboxdash.xbe "$R/hdd/partition2/"
echo "built $OUT/xboxdash.xbe; run it with $HERE/run.sh $WORK"
