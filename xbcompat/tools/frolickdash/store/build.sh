#!/bin/sh
# Builds frolick-store for the Sion (x86-64 Linux, static, no cgo) and
# copies it with its init script and installer into both Sion kits.
#   tools/frolickdash/store/build.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags='-s -w' -o frolick-store .
for k in sion game; do
	cp frolick-store sion/S97frolick-store sion/install-store.sh "$HERE/../dist/$k/"
	chmod 755 "$HERE/../dist/$k/frolick-store" "$HERE/../dist/$k/S97frolick-store" "$HERE/../dist/$k/install-store.sh"
done
rm frolick-store
