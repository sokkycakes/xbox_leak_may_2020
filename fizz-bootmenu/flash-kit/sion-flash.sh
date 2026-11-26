#!/bin/sh
#
# Flash the boot-menu ROM on the Sion (Acer CXI3, google/fizz), from Crimson,
# as root, from this folder:   sh sion-flash.sh
#
# 1. Reads the current BIOS region twice (the reads must match) and keeps it
#    as /data/sion/firmware-backup-<time>.rom. Copy that off the Sion. The ME
#    region can't be read from the OS (it's locked) and is never written, so
#    the backup holds the BIOS region and 0xff elsewhere: restore it with
#    --ifd -i bios, or with a clip, a layout that writes 0x200000-0xffffff.
# 2. Refuses unless the current flash is one of two known layouts (the new
#    ROM's own, or MrChromebox's older 5 MiB-BIOS fizz layout, whose regions
#    are moved to the new offsets) and write protect is off.
# 3. Carries over from the backup: RO_VPD (MAC address, serial),
#    SMMSTORE (UEFI settings and boot entries) and RW_MRC_CACHE.
# 4. Writes the BIOS region only (descriptor and ME untouched) and verifies.
# It does not reboot.
#
# Recovery if the Sion doesn't boot afterwards: flash the backup back with a
# SuzyQ cable (CCD) or an SPI clip on the flash chip.

set -e
D=$(cd "$(dirname "$0")" && pwd)
FR="$D/flashrom"
NEW="$D/bootmenu.rom"
T=$(date +%Y%m%d-%H%M%S)
W=/data/sion/flash-$T
B=/data/sion/firmware-backup-$T.rom

die() { echo "sion-flash: $*" >&2; exit 1; }
say() { echo "sion-flash: $*"; }

[ "$(id -u)" = 0 ] || die "run as root"
(cd "$D" && md5sum -c MD5SUMS > /dev/null) || die "kit files don't match MD5SUMS"
chmod +x "$FR"
mkdir -p "$W"

say "reading the current BIOS region (twice)"
"$FR" -p internal --ifd -i bios -r "$W/read1.rom" > "$W/read1.log" 2>&1 || { cat "$W/read1.log"; die "read failed"; }
"$FR" -p internal --ifd -i bios -r "$W/read2.rom" > "$W/read2.log" 2>&1 || { cat "$W/read2.log"; die "second read failed"; }
cmp -s "$W/read1.rom" "$W/read2.rom" || die "the two reads differ; not flashing"
cp "$W/read1.rom" "$B"
sync
say "backup: $B ($(md5sum < "$B" | cut -c1-32))"

[ "$(wc -c < "$B")" = "$(wc -c < "$NEW")" ] || die "flash size differs from the ROM"

# Where the regions are, in 4 KiB blocks. The new ROM (14 MiB BIOS):
# RW_MRC_CACHE 512+16, SMMSTORE 528+128, RO_VPD 656+4, FMAP 660.
# MrChromebox's older 5 MiB-BIOS fizz layout (what the Sion ran before):
# RW_MRC_CACHE 2816+16, SMMSTORE 2832+128, RO_VPD 2960+4, FMAP 2964.
# The new ROM's own layout goes over all of 0x200000-0xffffff, which the
# descriptor gives the BIOS region either way.
blk() { dd if="$1" bs=4096 skip="$2" count="$3" 2>/dev/null; }
sig() { blk "$1" "$2" 1 | head -c 8; }
[ "$(sig "$NEW" 660)" = "__FMAP__" ] || die "the new ROM has no FMAP where expected"
if [ "$(sig "$B" 660)" = "__FMAP__" ]; then
	blk "$B" 660 1 > "$W/fmap.old"
	blk "$NEW" 660 1 > "$W/fmap.new"
	cmp -s "$W/fmap.old" "$W/fmap.new" || die "the flash's FMAP differs from the new ROM's; not flashing (send $B to Claude)"
	OLD="512 528 656"
	say "current layout: same as the new ROM"
elif [ "$(sig "$B" 2964)" = "__FMAP__" ] && blk "$B" 2964 1 | grep -q "SI_BIOS" &&
		blk "$B" 2964 1 | grep -q "SMMSTORE" && blk "$B" 2964 1 | grep -q "RO_VPD"; then
	OLD="2816 2832 2960"
	say "current layout: MrChromebox 5 MiB BIOS (FMAP at 0xb94000); regions move to the new offsets"
else
	die "unknown flash layout; not flashing (send $B to Claude)"
fi
set -- $OLD
OMRC=$1 OSMM=$2 OVPD=$3
if [ "$(blk "$B" "$OVPD" 4 | tr -d '\377' | wc -c)" -gt 0 ]; then
	say "RO_VPD has data (MAC address and serial carried over)"
else
	say "warning: RO_VPD in the current flash is empty"
fi
"$FR" -p internal --wp-status > "$W/wp.log" 2>&1 || true
if grep -qi "protection mode: hardware\|protection range: start=0x[0-9a-f]* length=0x0*[1-9a-f]" "$W/wp.log"; then
	cat "$W/wp.log"; die "write protect looks enabled; not flashing"
fi

cp "$NEW" "$W/merged.rom"
for r in "$OMRC 512 16" "$OSMM 528 128" "$OVPD 656 4"; do
	set -- $r
	blk "$B" "$1" "$3" | dd of="$W/merged.rom" bs=4096 seek="$2" count="$3" conv=notrunc 2>/dev/null
done
say "merged ROM: $W/merged.rom ($(md5sum < "$W/merged.rom" | cut -c1-32))"

say "writing the BIOS region"
"$FR" -p internal --ifd -i bios -N -w "$W/merged.rom" > "$W/write.log" 2>&1 || { tail -20 "$W/write.log"; die "WRITE FAILED: do not power off; run '$FR -p internal --ifd -i bios -N -w $B' to put the backup back"; }
"$FR" -p internal --ifd -i bios -N -v "$W/merged.rom" > "$W/verify.log" 2>&1 || { tail -20 "$W/verify.log"; die "VERIFY FAILED: do not power off; run '$FR -p internal --ifd -i bios -N -w $B' to put the backup back"; }
sync
say "flashed and verified. Reboot to start the boot menu. Logs and the merged ROM: $W"
