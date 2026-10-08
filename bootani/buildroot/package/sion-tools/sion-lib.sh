# Shared by S03sion, S41sion-net, sion-netd and sion.
#
# The boot disk has two partitions:
#   1  SIONBOOT  FAT EFI system partition: EFI/BOOT/BOOTX64.EFI (the whole
#                system: kernel + root filesystem), EFI/sion/ (previous and
#                trial images), sion/ (settings, keys, logs), theseus/
#   2  SIONDATA  ext4, mounted on /data (persistent; the root filesystem
#                itself lives in RAM and starts fresh at every boot)
#
# The EFI partition is mounted only while it is read or written, each time on
# a private mount point, so pulling the stick (or the power) leaves a clean
# FAT behind and two writers never unmount each other.

SION_RUN=/var/run/sion
SION_DATA=/data
SION_LOG=/var/log/sion.log
EFI_GLOBAL=8be4df61-93ca-11d2-aa0d-00e098032b8c
EFIVARS=/sys/firmware/efi/efivars

mkdir -p "$SION_RUN"

log() {
	echo "$(date '+%F %T') $*" >> "$SION_LOG"
}

# All partitions, as /dev paths.
partitions() {
	for p in /sys/class/block/*; do
		[ -e "$p/partition" ] && echo "/dev/${p##*/}"
	done
}

fs_label() {
	blkid -s LABEL -o value "$1" 2>/dev/null
}

# The disk a partition is on, and the partition's number.
part_disk() {
	d=$(readlink -f "/sys/class/block/${1##*/}/..")
	echo "/dev/${d##*/}"
}
part_num() {
	cat "/sys/class/block/${1##*/}/partition"
}

# The partition UUID UEFI booted this system from, from the BootCurrent entry
# (empty when the entry names a whole USB device, as removable-media entries
# often do).
boot_partuuid() {
	cur=$(efi_boot_current) || return 1
	efibootmgr -v 2>/dev/null | sed -n "s/^Boot$cur[* ].*HD([0-9]*,GPT,\([0-9a-fA-F-]*\),.*/\1/p" | head -n1
}

# Find the EFI partition this system booted from, once; later calls read it
# from $SION_RUN/esp-dev. Prefer the partition UEFI names, then a SIONBOOT
# label, then (older sticks) a FAT partition with a theseus/ folder.
find_esp() {
	if [ -r "$SION_RUN/esp-dev" ]; then
		cat "$SION_RUN/esp-dev"
		return 0
	fi
	want=$(boot_partuuid | tr 'A-F' 'a-f')
	# The kernel enumerates USB disks a moment after init starts.
	for _ in $(seq 1 50); do
		found=
		for dev in $(partitions); do
			if [ -n "$want" ] && [ "$(blkid -s PARTUUID -o value "$dev" 2>/dev/null)" = "$want" ]; then
				found=$dev
				break
			fi
			[ -z "$found" ] && [ "$(fs_label "$dev")" = SIONBOOT ] && found=$dev
		done
		if [ -z "$found" ]; then
			for dev in $(partitions); do
				[ "$(blkid -s TYPE -o value "$dev" 2>/dev/null)" = vfat ] || continue
				m=$(esp_mount_dev "$dev") || continue
				[ -d "$m/theseus" ] && found=$dev
				esp_umount "$m"
				[ -n "$found" ] && break
			done
		fi
		if [ -n "$found" ]; then
			echo "$found" > "$SION_RUN/esp-dev"
			echo "$found"
			return 0
		fi
		sleep 0.2
	done
	return 1
}

# The SIONDATA partition on the same disk as the EFI partition.
find_data() {
	esp=$(find_esp) || return 1
	disk=$(part_disk "$esp")
	for dev in $(partitions); do
		[ "$(part_disk "$dev")" = "$disk" ] || continue
		[ "$(fs_label "$dev")" = SIONDATA ] && echo "$dev" && return 0
	done
	return 1
}

esp_mount_dev() {
	m=$(mktemp -d "$SION_RUN/esp.XXXXXX") || return 1
	if mount -t vfat -o rw,noatime,flush "$1" "$m" 2>/dev/null; then
		echo "$m"
	else
		rmdir "$m"
		return 1
	fi
}

# Prints a fresh mount point of the EFI partition; pair with esp_umount.
esp_mount() {
	esp=$(find_esp) || return 1
	esp_mount_dev "$esp"
}

esp_umount() {
	sync
	umount "$1" 2>/dev/null || umount -l "$1"
	rmdir "$1" 2>/dev/null
}

# Settings from sion/sion.conf on the EFI partition, copied at boot (with
# Windows line endings removed) by S03sion.
load_conf() {
	HOSTNAME=sion
	SSH=yes
	WIFI_SSID=
	WIFI_PASSWORD=
	WIFI_COUNTRY=
	WIFI_ADDRESS=
	WIFI_GATEWAY=
	WIFI_DNS=
	PASSWORD=
	# shellcheck source=/dev/null
	[ -r "$SION_RUN/sion.conf" ] && . "$SION_RUN/sion.conf"
}

# UEFI variables as hex boot entry numbers (XXXX).
efi_var_u16() {
	f="$EFIVARS/$1-$EFI_GLOBAL"
	[ -r "$f" ] || return 1
	# 4 bytes of attributes, then the little-endian value. Read it whole:
	# efivarfs doesn't support seeking past the attributes.
	cat "$f" | od -An -tx1 | tr -s ' \n' '  ' | awk '{ printf "%s%s\n", toupper($6), toupper($5) }'
}
efi_boot_current() {
	efi_var_u16 BootCurrent
}

mount_efivars() {
	[ -d /sys/firmware/efi ] || return 1
	mountpoint -q "$EFIVARS" || mount -t efivarfs efivarfs "$EFIVARS" 2>/dev/null
}
