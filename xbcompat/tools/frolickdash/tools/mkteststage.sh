#!/bin/bash
# Stage a test console for the Frolick build under xbcompat: games to launch
# and saved games to manage, so the belt and the Memory pages can be tried
# in the cloud without a real disk.
#
#   tools/frolickdash/tools/mkteststage.sh [WORK]     (after build.sh WORK)
#
# Into WORK/frolick/run (the hard disk run.sh boots):
#   E:\Games\<name>\default.xbe   three games: Frolick's own image rebuilt
#                                 with its own title ID, name and title
#                                 image (imagebld /TITLEIMAGE), so each has
#                                 a cover and launching one boots the hub
#                                 (EXTRA_GAMES=n adds n more, for the
#                                 All games grid)
#   disc\default.xbe              one of them in the disc tray
#   E:\UDATA\<title>\             TitleMeta.xbx, TitleImage.xbx and saves
#                                 (SaveMeta.xbx, SaveImage.xbx) for two of
#                                 the games and for a title with no game
# The images are coloured squares with the name on them, made with PIL
# and packed by the leak's bundler (XPR, as the dashboard reads them).
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-/tmp/xbdash}
OUT="$WORK/frolick"
R="$OUT/run"
T="$OUT/teststage"
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$WORK/wineprefix}
W=$(echo "Z:$WORK" | tr / '\\')
mkdir -p "$T"

# An XPR texture of a coloured square with text, size px.
xpr() { # name size r g b text out.xbx
    python3 - "$T/$1.tga" "$2" "$3" "$4" "$5" "$6" <<'PY'
import sys
from PIL import Image, ImageDraw
out, size, r, g, b, text = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
im = Image.new("RGBA", (size, size), (r, g, b, 255))
d = ImageDraw.Draw(im)
d.rectangle([3, 3, size - 4, size - 4], outline=(255, 255, 255, 255), width=2)
w = d.textlength(text)
d.text(((size - w) / 2, size / 2 - 6), text, fill=(255, 255, 255, 255))
im.save(out)
PY
    printf 'Texture Tex\n{\n    Source %s.tga\n    Format D3DFMT_A8R8G8B8\n    Width %s\n    Height %s\n    Levels 1\n}\n' "$1" "$2" "$2" > "$T/$1.rdf"
    (cd "$T" && timeout 120 wine "$WORK/xb/public/idw/bundler.exe" "$1.rdf" -o "$1.xbx" > /dev/null)
    [ "$T/$1.xbx" -ef "$7" ] || cp "$T/$1.xbx" "$7"
}

# A settings file the dashboard reads (CSettingsFile: UTF-16 text, Name=Value lines).
meta() { # out.xbx line...
    python3 - "$@" <<'PY'
import sys
open(sys.argv[1], "wb").write(("\r\n".join(sys.argv[2:]) + "\r\n").encode("utf-16"))
PY
}

# A game: Frolick's image with its own identity and title image.
game() { # folder "Name" titleid r g b
    local dir="$R/hdd/partition1/Games/$1"
    mkdir -p "$dir"
    xpr "title_$1" 128 "$4" "$5" "$6" "$2" "$T/title_$1.xbx"
    ins=""
    for l in english:English japanese:Japanese german:German french:French spanish:Spanish italian:Italian; do
        ins="$ins /INSERTFILE:$W\\dash\\${l%%:*}.txt,${l##*:}Xlate,N"
    done
    (cd "$OUT" && timeout 300 wine "$WORK/xb/public/idw/imagebld.exe" /TESTID:0x$3 /TESTRATINGS:0xFFFFFFFF \
        /TESTREGION:0x7FFFFFFF "/TESTNAME:$2" /INITFLAGS:0x00000004 /LIMITMEM \
        "/INSERTFILE:$W\\frolick\\dash\\obj\\xipsums.bin,XIPS,R" $ins /TESTMEDIATYPES:1 /NOLIBWARN /NOSETUPHD \
        "/TITLEIMAGE:$W\\frolick\\teststage\\title_$1.xbx" \
        /INITFLAGS:0x00000000 /IN:xboxdash.exe "/OUT:$W\\frolick\\teststage\\$1.xbe" | tr -d '\r' | grep -v "^$" || true)
    cp "$T/$1.xbe" "$dir/default.xbe"
    cp "$OUT/dash/default.xip" "$dir/"
}

# A title's saved games: UDATA\<id> with its meta and image, then each
# save's folder with its own meta and image.
title() { # titleid "Title Name" r g b
    local dir="$R/hdd/partition1/UDATA/$1"
    mkdir -p "$dir"
    meta "$dir/TitleMeta.xbx" "TitleName=$2"
    xpr "t_$1" 128 "$3" "$4" "$5" "$2" "$dir/TitleImage.xbx"
}
save() { # titleid savedir "Save Name" r g b [bytes]
    local dir="$R/hdd/partition1/UDATA/$1/$2"
    mkdir -p "$dir"
    meta "$dir/SaveMeta.xbx" "Name=$3"
    xpr "s_$1_$2" 64 "$4" "$5" "$6" "$3" "$dir/SaveImage.xbx"
    head -c "${7:-20000}" /dev/urandom > "$dir/savegame.dat"
}

rm -rf "$R/hdd/partition1/Games" "$R/hdd/partition1/UDATA" "$R/disc"
mkdir -p "$R/disc"
game GameA "Test Game A" 54455354 40 90 160
game GameB "Another Game" 54455355 160 70 40
game GameC "Third Game" 54455356 60 140 70
for i in $(seq 1 "${EXTRA_GAMES:-0}"); do
    game "Extra$i" "Extra Game $i" "$(printf '545546%02X' "$i")" $((40 + i * 37 % 160)) $((60 + i * 53 % 140)) $((80 + i * 71 % 150))
done
cp "$R/hdd/partition1/Games/GameC/default.xbe" "$R/disc/default.xbe"
cp "$OUT/dash/default.xip" "$R/disc/"

title 54455354 "Test Game A" 40 90 160
save 54455354 0123456789ABCDEF "Level 3" 60 110 180
save 54455354 FEDCBA9876543210 "Boss Fight" 90 60 180 60000
save 54455354 AAAA000011112222 "Autosave" 120 120 120
title 54455355 "Another Game" 160 70 40
save 54455355 1111222233334444 "Chapter 2" 200 100 60
title 4D530064 "Lost Title" 120 60 120
save 4D530064 5555666677778888 "Old Save" 150 90 150
echo "staged $R: games, a disc and saved games"
