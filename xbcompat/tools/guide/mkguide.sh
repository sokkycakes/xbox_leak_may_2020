#!/bin/sh
# Builds the Guide overlay's asset directory (xbcompat looks for "guide"
# beside its binary, or XBCOMPAT_GUIDE_DIR) from the 360 packages already run
# through xurconv.py (/mnt/project-files/xui360/blades-converted/).
#   mkguide.sh CONVERTED_DIR OUT_DIR [FONT.ttf]
set -e
C=$1; O=$2; F=${3:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}
T=$(cd "$(dirname "$0")/../frolickdash/tools" && pwd)
G=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$O"
python3 "$T/xur2xui.py" "$C/huduiskin_skin.xzp" skin.xur "$O/hudskin.xui"
python3 "$T/xur2xui.py" "$C/xam_xam.xzp" hudbkgnd.xur "$O/hudbkgnd.xui"
python3 "$T/xur2xui.py" "$C/hud_hud.xzp" MainMenuSignedOut.xur "$O/GuideMain.xui"
python3 "$T/xur2xui.py" "$C/hud_hud.xzp" Options.xur "$O/GuideSettings.xui"
python3 "$G/extract_png.py" "$O" "$C/huduiskin_skin.xzp" "$C/hud_hud.xzp" "$C/xam_xam.xzp" "$C/xam_shrdres.xzp"
rm -f "$O"/*.png
cp "$F" "$O/font.ttf"
