#!/bin/sh
#
# Try the boot menu on the Sion without flashing: crimsonboot.efi (the menu's
# LinuxBoot kernel built as an EFI app, initramfs and command line inside)
# goes to EFI/sion/ on the boot partition, with a UEFI entry used for the
# NEXT BOOT ONLY (BootNext). The boot order and the firmware don't change, so
# the boot after that is the normal one again.
#
#   sh sion-install.sh            install and arm one boot
#   sh sion-install.sh --again    arm one more boot (already installed)
#   sh sion-install.sh --remove   delete the entry and the file
#
# Run as root on the Sion from the kit folder. Nothing restarts; reboot
# yourself when ready. On the test boot the menu's "UEFI (edk2)" entry only
# reboots (today's firmware doesn't read the CMOS byte yet).

. /usr/lib/sion/sion-lib.sh

D=$(cd "$(dirname "$0")" && pwd)
EFI=crimsonboot.efi
LABEL="Crimson boot menu"
LOADER='\EFI\sion\crimsonboot.efi'

die() { echo "bootmenu-test: $*" >&2; exit 1; }
say() { echo "bootmenu-test: $*"; log "bootmenu-test: $*"; }

[ "$(id -u)" = 0 ] || die "run as root"
mount_efivars || die "no UEFI variables"
command -v efibootmgr > /dev/null || die "no efibootmgr"
esp=$(find_esp) || die "boot partition not found"

entries() {
	efibootmgr 2>/dev/null | sed -n "s/^Boot\([0-9A-Fa-f]\{4\}\)\*\{0,1\} $LABEL\(	.*\)\{0,1\}\$/\1/p"
}

if [ "$1" = --remove ]; then
	for e in $(entries); do efibootmgr -q -b "$e" -B; done
	m=$(esp_mount) && { rm -f "$m/EFI/sion/$EFI"; esp_umount "$m"; }
	say "removed"
	exit 0
fi

if [ "$1" != --again ]; then
	(cd "$D" && md5sum -c MD5SUMS > /dev/null) || die "kit files don't match MD5SUMS"
	m=$(esp_mount) || die "can't mount the boot partition"
	mkdir -p "$m/EFI/sion"
	cp "$D/$EFI" "$m/EFI/sion/$EFI.new" && mv "$m/EFI/sion/$EFI.new" "$m/EFI/sion/$EFI" || { esp_umount "$m"; die "copying failed"; }
	sync
	cmp -s "$D/$EFI" "$m/EFI/sion/$EFI" || { esp_umount "$m"; die "the copy doesn't match"; }
	esp_umount "$m"
fi

entry=$(entries | head -n1)
if [ -z "$entry" ]; then
	order=$(efibootmgr | sed -n 's/^BootOrder: //p')
	efibootmgr -q -c -d "$(part_disk "$esp")" -p "$(part_num "$esp")" -L "$LABEL" -l "$LOADER" || die "creating the entry failed"
	entry=$(entries | head -n1)
	[ -n "$entry" ] || die "the new entry didn't stick"
	# efibootmgr -c puts it first; keep the old order.
	if [ -n "$order" ]; then efibootmgr -q -o "$order"; fi
fi
efibootmgr -q -n "$entry" || die "setting BootNext failed"
say "next boot only: Boot$entry ($LOADER); the boot order is unchanged"
efibootmgr | grep -i "^Boot\(Next\|Order\|$entry\)"
