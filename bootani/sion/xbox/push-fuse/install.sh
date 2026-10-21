#!/bin/sh
# Installs the FUSE and squashfs modules built for the Sion's 6.18.7 kernel.
# Run as root on the Sion. Copies them into the persistent /data/overlay so they
# survive reboot, loads them now with insmod (the Sion has no depmod), and adds a
# boot init script that loads fuse. overlay is not included: its backing_file
# helpers aren't built into this kernel, so it needs a kernel rebuild.
set -e
D=$(cd "$(dirname "$0")" && pwd)
cd "$D"
md5sum -c MD5SUMS
M=/data/overlay/lib/modules/6.18.7/extra
mkdir -p "$M" /data/overlay/etc/init.d
cp fuse.ko squashfs.ko "$M/"
cp S06fuse /data/overlay/etc/init.d/S06fuse
chmod 755 /data/overlay/etc/init.d/S06fuse
grep -q '^fuse ' /proc/modules || insmod "$M/fuse.ko"
grep -q '^squashfs ' /proc/modules || insmod "$M/squashfs.ko"
test -c /dev/fuse && echo "fuse installed; /dev/fuse present"
