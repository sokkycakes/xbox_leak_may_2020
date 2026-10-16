#!/bin/sh
# Link a Buildroot output dir to the fixed paths the rgds device profile uses,
# and write target .pc files whose paths point into the sysroot. Using these
# instead of PKG_CONFIG_SYSROOT_DIR keeps the host Qt build on host packages.
# usage: setup-sdk.sh <buildroot output dir>
set -e
out=$(readlink -f "$1")
sdk=/opt/rgds-sdk
mkdir -p $sdk
ln -sfn "$out/host" $sdk/host
ln -sfn "$(readlink -f "$out/staging")" $sdk/sysroot
rm -rf $sdk/pkgconfig
mkdir -p $sdk/pkgconfig
for pc in $sdk/sysroot/usr/lib/pkgconfig/*.pc $sdk/sysroot/usr/share/pkgconfig/*.pc; do
    [ -f "$pc" ] || continue
    sed -e "s#=/usr#=$sdk/sysroot/usr#" "$pc" > $sdk/pkgconfig/$(basename "$pc")
done
