#!/bin/sh
# Builds frolick-store for the Sion (x86-64 Linux, static, no cgo) and
# copies it with its init script and installer into both Sion kits.
#   tools/frolickdash/store/build.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
REV=$(git -C "$HERE" rev-parse --short=8 HEAD 2>/dev/null || echo dev)
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath \
	-ldflags="-s -w -X main.buildRev=$REV -X main.buildDate=$(date -u +%Y-%m-%d)" -o frolick-store .
# the display keeper with the resolution switch (Console Settings > Display)
gcc -static -O2 -Wall -o xbox-av "$HERE/../../sion/av/xbox-av.c"
for k in sion game; do
	cp frolick-store sion/S97frolick-store sion/install-store.sh xbox-av sion/install-xbox-av.sh "$HERE/../dist/$k/"
	chmod 755 "$HERE/../dist/$k/frolick-store" "$HERE/../dist/$k/S97frolick-store" "$HERE/../dist/$k/install-store.sh" \
		"$HERE/../dist/$k/xbox-av" "$HERE/../dist/$k/install-xbox-av.sh"
done
rm frolick-store xbox-av
