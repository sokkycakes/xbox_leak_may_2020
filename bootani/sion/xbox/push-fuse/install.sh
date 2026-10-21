#!/bin/sh
# Installs the FUSE, overlay and squashfs modules built for the Sion's 6.18.7 kernel.
# Run as root on the Sion (over ssh). Copies into the persistent /data/overlay so
# they survive reboot, loads fuse now, and adds an init script that loads it at boot.
set -e
D=$(cd "$(dirname "$0")" && pwd)
cd "$D"
md5sum -c MD5SUMS
M=/data/overlay/lib/modules/6.18.7/extra
mkdir -p "$M" /data/overlay/etc/init.d
cp fuse.ko overlay.ko squashfs.ko "$M/"
cp S06fuse /data/overlay/etc/init.d/S06fuse
chmod 755 /data/overlay/etc/init.d/S06fuse
depmod -a 6.18.7
modprobe fuse
test -c /dev/fuse && echo "fuse installed; /dev/fuse present"
