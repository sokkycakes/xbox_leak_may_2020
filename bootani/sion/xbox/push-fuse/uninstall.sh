#!/bin/sh
# Reverts push-fuse: unloads fuse and removes the files install.sh added.
set -e
rmmod fuse 2>/dev/null || true
rm -f /data/overlay/lib/modules/6.18.7/extra/fuse.ko /data/overlay/lib/modules/6.18.7/extra/overlay.ko /data/overlay/lib/modules/6.18.7/extra/squashfs.ko
rm -f /data/overlay/etc/init.d/S06fuse
echo "push-fuse removed"
