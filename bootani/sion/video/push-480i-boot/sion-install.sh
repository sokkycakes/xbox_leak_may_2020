#!/bin/sh
#
# push-480i-boot: the whole boot at 15 kHz on the image the Sion runs now,
# without a new image. Run as root on the Sion, from the kit folder:
#
#   sh sion-install.sh            install
#   sh sion-install.sh --remove   back to the plain boot entry
#
# The image's kernel has video=720x480@60 built in, so its text console
# (boot messages, and the tty whenever no program holds the screen) runs at
# 720x480p, 31 kHz, and the boot animation starts before sion-video can set
# 480i. Two things fix that from the boot entry, which the kernel reads after
# its built-in command line:
#
#   video=HDMI-A-1:2880x480M@30i   the console gets a 2880x480 interlaced
#       framebuffer from the start: 15.1 kHz, 59 fields (the kernel's CVT
#       timing; a named video= beats the built-in global one). Its GTF
#       timing, and CVT at @60i, come out at 31 kHz, so neither is used.
#   initrd=\sion\video-boot.cpio   unpacked over the image's own files at
#       boot: sion-video, its EDID, S00bootani running `sion-video early`
#       before the boot animation, and bootani filling a 2880x480i screen.
#
# Nothing restarts; it applies from the next boot. If the firmware can't load
# the entry, it falls back to its own SATA entry, which boots the image as
# before. An image built with the 15 kHz kernel (board/sion/linux.fragment)
# doesn't need this: run --remove when installing it.
#

. /usr/lib/sion/sion-lib.sh

D=$(cd "$(dirname "$0")" && pwd)
CPIO=video-boot.cpio
OPTS='initrd=\sion\video-boot.cpio video=HDMI-A-1:2880x480M@30i'
LABEL=Sion
LOADER='\EFI\BOOT\BOOTX64.EFI'

die() {
	echo "push-480i-boot: $*" >&2
	exit 1
}

say() {
	echo "push-480i-boot: $*"
	log "push-480i-boot: $*"
}

[ "$(id -u)" = 0 ] || die "run as root"
mount_efivars || die "no UEFI variables"
command -v efibootmgr > /dev/null || die "no efibootmgr"
esp=$(find_esp) || die "boot partition not found"
disk=$(part_disk "$esp")
part=$(part_num "$esp")
puuid=$(blkid -s PARTUUID -o value "$esp" | tr 'A-F' 'a-f')
[ -n "$puuid" ] || die "no PARTUUID for $esp"

# "Sion" entries that boot this partition's image: 0005 ...
entries_here() {
	efibootmgr -v 2>/dev/null | tr 'A-F' 'a-f' | grep -i "^boot[0-9a-f]\{4\}\*\{0,1\} sion[	 ]" |
		grep "$puuid" | sed 's/^boot\([0-9a-f]\{4\}\).*/\1/' | tr 'a-f' 'A-F'
}

# Replaces this partition's "Sion" entries with one carrying $1 as its
# options (none when empty), first in the boot order.
set_entry() {
	old=$(entries_here)
	if [ -n "$1" ]; then
		efibootmgr -q -c -d "$disk" -p "$part" -L "$LABEL" -l "$LOADER" -u "$1" || die "creating the boot entry failed"
	else
		efibootmgr -q -c -d "$disk" -p "$part" -L "$LABEL" -l "$LOADER" || die "creating the boot entry failed"
	fi
	new=
	for e in $(entries_here); do
		case " $old " in *" $e "*) ;; *) new=$e ;; esac
	done
	[ -n "$new" ] || die "the new boot entry didn't stick; the old one is unchanged"
	order=$(efibootmgr | sed -n 's/^BootOrder: //p')
	[ "${order%%,*}" = "$new" ] || efibootmgr -q -o "$new${order:+,$order}" || die "setting the boot order failed"
	for e in $old; do
		efibootmgr -q -b "$e" -B
	done
	say "boot entry Boot$new: $LOADER${1:+ $1}"
}

if [ "$1" = --remove ]; then
	set_entry ""
	m=$(esp_mount) && {
		rm -f "$m/sion/$CPIO"
		esp_umount "$m"
	}
	say "removed; the next boot is the image's own (console 720x480p)"
	efibootmgr -v | grep -i "^Boot\|^BootOrder"
	exit 0
fi

(cd "$D" && md5sum -c MD5SUMS > /dev/null) || die "kit files don't match MD5SUMS"

# The add-on files on the EFI partition, checked after copying.
m=$(esp_mount) || die "can't mount the boot partition"
mkdir -p "$m/sion"
cp "$D/$CPIO" "$m/sion/$CPIO.new" && mv "$m/sion/$CPIO.new" "$m/sion/$CPIO" || { esp_umount "$m"; die "copying $CPIO failed"; }
sync
cmp -s "$D/$CPIO" "$m/sion/$CPIO" || { esp_umount "$m"; die "$CPIO on the boot partition doesn't match"; }
esp_umount "$m"

set_entry "$OPTS"

# The running system and /data/overlay get the same sion-video (the overlay
# copy is what runs once S03 has applied the overlay).
T=$(mktemp -d)
(cd "$T" && cpio -id --quiet usr/libexec/sion-video < "$D/$CPIO")
if ! cmp -s "$T/usr/libexec/sion-video" /usr/libexec/sion-video; then
	cp /usr/libexec/sion-video /usr/libexec/sion-video.pre-480i-boot 2>/dev/null
	install -m 0755 "$T/usr/libexec/sion-video" /usr/libexec/sion-video
	sion persist /usr/libexec/sion-video > /dev/null
	[ -e /usr/libexec/sion-video.pre-480i-boot ] && sion persist /usr/libexec/sion-video.pre-480i-boot > /dev/null
fi
rm -rf "$T"

say "installed; the next boot is 15 kHz from the kernel's first frame (nothing restarted)"
efibootmgr -v | grep -i "^Boot\|^BootOrder"
