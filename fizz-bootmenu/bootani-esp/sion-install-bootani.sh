#!/bin/sh
#
# Put the boot animation the LinuxBoot firmware plays on the Sion's SIONBOOT
# partition, as sion/bootani/ (bootani, a trimmed Intel Mesa and the glibc
# they run with, about 16 MB). Run on the Sion as root, from this folder:
#
#   sh sion-install-bootani.sh            install or update
#   sh sion-install-bootani.sh --remove   take it off (LinuxBoot then boots
#                                         Crimson without an animation)
#
# It finds SIONBOOT by the sion.esp= the firmware passed (the partition this
# boot came from), else by its label. Nothing else on the partition changes.

set -e
D=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

dev=
esp=$(sed -n 's/.*sion\.esp=\([^ ]*\).*/\1/p' /proc/cmdline)
[ -n "$esp" ] && [ -e "/dev/disk/by-partuuid/$esp" ] && dev=$(readlink -f "/dev/disk/by-partuuid/$esp")
if [ -z "$dev" ]; then
	for p in /sys/class/block/*; do
		n=${p##*/}
		[ -e "$p/partition" ] || continue
		if [ -n "$esp" ] && grep -qix "PARTUUID=$esp" "$p/uevent"; then dev=/dev/$n; break; fi
	done
fi
[ -z "$dev" ] && [ -e /dev/disk/by-label/SIONBOOT ] && dev=$(readlink -f /dev/disk/by-label/SIONBOOT)
[ -z "$dev" ] && command -v blkid >/dev/null && dev=$(blkid -L SIONBOOT 2>/dev/null || true)
[ -n "$dev" ] || { echo "can't find the SIONBOOT partition" >&2; exit 1; }

mnt=$(awk -v d="$dev" '$1 == d { print $2; exit }' /proc/mounts)
own=
if [ -z "$mnt" ]; then
	mnt=$(mktemp -d)
	mount -t vfat "$dev" "$mnt"
	own=1
elif awk -v m="$mnt" '$2 == m && $4 ~ /(^|,)ro(,|$)/ { r = 1 } END { exit !r }' /proc/mounts; then
	mount -o remount,rw "$mnt"
	own=ro
fi
cleanup() {
	sync
	case "$own" in
	1) umount "$mnt"; rmdir "$mnt" ;;
	ro) mount -o remount,ro "$mnt" ;;
	esac
}
trap cleanup EXIT

[ -f "$mnt/EFI/BOOT/BOOTX64.EFI" ] || { echo "$dev doesn't look like SIONBOOT" >&2; exit 1; }
echo "SIONBOOT: $dev (at $mnt)"

if [ "$1" = --remove ]; then
	rm -rf "$mnt/sion/bootani" "$mnt/sion/bootani.new"
	echo "Removed sion/bootani."
	exit 0
fi

(cd "$D" && md5sum -c MD5SUMS >/dev/null) || { echo "the kit's files don't match MD5SUMS" >&2; exit 1; }
need=$(du -sk "$D/sion/bootani" | cut -f1)
free=$(df -k "$mnt" | awk 'NR == 2 { print $4 }')
old=$(du -sk "$mnt/sion/bootani" 2>/dev/null | cut -f1)
[ $((free + ${old:-0})) -gt $((need + 1024)) ] || { echo "not enough room on SIONBOOT: ${free} KB free, ${need} KB needed" >&2; exit 1; }

# Copy next to the old one, check it, then swap, so a power cut leaves
# either the old animation or the new one.
rm -rf "$mnt/sion/bootani.new"
mkdir -p "$mnt/sion/bootani.new"
cp -r "$D/sion/bootani/." "$mnt/sion/bootani.new/"
sync
(cd "$mnt" && sed 's| sion/bootani/| sion/bootani.new/|' "$D/MD5SUMS" | md5sum -c - >/dev/null) ||
	{ echo "copy check failed; nothing changed" >&2; rm -rf "$mnt/sion/bootani.new"; exit 1; }
rm -rf "$mnt/sion/bootani"
mv "$mnt/sion/bootani.new" "$mnt/sion/bootani"
echo "Installed sion/bootani ($need KB). It plays from the next power-on."
