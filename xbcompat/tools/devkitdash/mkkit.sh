#!/bin/bash
# Assemble the Sion kit from build.sh's output into DIR (default dist/sion
# beside this script):
#   tools/devkitdash/mkkit.sh [WORK] [DIR]
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
WORK=${1:-/tmp/devkitdash}
OUT=${2:-$HERE/dist/sion}
PKG="$ROOT/bootani/buildroot/package/xbcompat"
rm -rf "$OUT"
mkdir -p "$OUT/media"
cp "$WORK/devkit/devkitdash.xbe" "$OUT/"
cp "$WORK"/devkit/media/* "$OUT/media/"
# The HLE map: which library functions xbcompat replaces, found in the XBE
# itself (independent of the xbcompat binary).
cp "$(python3 "$ROOT/xbcompat/tools/xbrun.py" --map-only "$WORK/devkit/devkitdash.xbe")" "$OUT/devkitdash.map"
chmod 644 "$OUT/devkitdash.map"
cp "$PKG/xbox-hostinfo" "$PKG/xbox-bluetooth" "$PKG/xbox-session" "$OUT/"
# The session change as a patch too, for an image whose session differs
# from this tree's, with the hash of the session it was made against.
cp "$HERE/kit/xbox-session.patch" "$HERE/kit/xbox-session.base.sha256" "$OUT/"
cp "$HERE"/kit/*.sh "$HERE/kit/README.md" "$OUT/"
(cd "$OUT" && sha256sum devkitdash.xbe devkitdash.map xbox-hostinfo xbox-bluetooth xbox-session xbox-session.patch \
    xbox-session.base.sha256 install-devkit.sh media/* > SHA256SUMS)
echo "kit in $OUT"
