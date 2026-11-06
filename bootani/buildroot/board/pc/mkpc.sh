#!/bin/bash
# The generic x86-64 PC test image: the Sion's system (boot animation and
# dashboard under xbcompat, sion-tools) made to boot on any UEFI PC, built
# the fast way (no full Buildroot run) from an existing Sion image's root
# filesystem:
#
#   mkpc.sh BASE_ROOT BUNDLE LINUX_TREE FIRMWARE OUT_DIR
#
#   BASE_ROOT   a Sion image's root filesystem, unpacked (the initramfs cpio
#               inside BOOTX64.EFI; see docs in sion-image-fast-rebuild)
#   BUNDLE      xbcompat-sion.tar.gz from `MESA=system tools/sion/bundle.sh`
#               (Ubuntu's i386 Mesa: radeonsi, r600, nouveau, iris, crocus,
#               llvmpipe...)
#   LINUX_TREE  linux-6.18.7 configured with x86_64_defconfig +
#               board/common + board/pc fragments, bzImage and modules built
#   FIRMWARE    a tree with lib/firmware (xz-compressed linux-firmware
#               subset: amdgpu, radeon, nvidia GSP, i915/xe, Wi-Fi, NICs)
#   OUT_DIR     gets BOOTX64.EFI and crimson-pc-test.img
#
# The stick's partitions are CRIMSONBOOT and CRIMSONDATA, not SIONBOOT and
# SIONDATA, and the image only ever uses those (/etc/sion-labels), so put
# into a Sion it can't touch the Sion's own disk or boot entries.
set -e
BASE=$1 BUNDLE=$2 LINUX=$3 FW=$4 OUT=$5
HERE=$(cd "$(dirname "$0")" && pwd)
EXT=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$EXT/../.." && pwd)
P=$EXT/package
W=$OUT/work
ROOT=$W/root
mkdir -p "$OUT" "$W"
rm -rf "$ROOT"
cp -a "$BASE" "$ROOT"

# ---- xbcompat, the boot animation and the dashboard -------------------------
rm -rf "$ROOT/opt/xbcompat"
tar -C "$ROOT/opt" -xzf "$BUNDLE"
# The Guide's assets (the Sion gets them from push-guide).
tar -C "$ROOT/opt/xbcompat/bin" -xzf "$REPO/bootani/sion/xbox/push-guide/guide.tar.gz"
install -D -m 0755 "$P/xbcompat/xbox-boot" "$ROOT/usr/libexec/xbox-boot"
install -D -m 0755 "$P/xbcompat/xbox-session" "$ROOT/usr/libexec/xbox-session"
install -D -m 0755 "$P/xbcompat/xbox-title" "$ROOT/usr/libexec/xbox-title"
install -D -m 0755 "$P/xbcompat/xbox-env" "$ROOT/usr/lib/xbox/xbox-env"
install -D -m 0755 "$P/xbcompat/dashboard" "$ROOT/usr/libexec/dashboard"
install -D -m 0755 "$P/xbcompat/S98xbox" "$ROOT/etc/init.d/S98xbox"
install -D -m 0755 "$REPO/xbcompat/tools/sion/cards/sion-cards" "$ROOT/usr/sbin/sion-cards"
install -D -m 0755 "$REPO/xbcompat/tools/sion/cards/S35cards" "$ROOT/etc/init.d/S35cards"
echo 'DASHBOARD="xbox"' > "$ROOT/etc/default/dashboard"

# ---- sion-tools, with this image's own labels -------------------------------
install -D -m 0755 "$P/sion-tools/sion" "$ROOT/usr/sbin/sion"
install -D -m 0644 "$P/sion-tools/sion-lib.sh" "$ROOT/usr/lib/sion/sion-lib.sh"
install -D -m 0755 "$P/sion-tools/sion-netd" "$ROOT/usr/libexec/sion-netd"
install -D -m 0755 "$P/sion-tools/sion-hwlog" "$ROOT/usr/libexec/sion-hwlog"
install -D -m 0600 "$P/sion-tools/sshd_config" "$ROOT/etc/ssh/sshd_config"
install -D -m 0644 "$P/sion-tools/sion.conf" "$ROOT/usr/share/sion/sion.conf"
install -D -m 0644 "$P/sion-tools/README.txt" "$ROOT/usr/share/sion/README.txt"
install -D -m 0755 "$P/sion-tools/S03sion" "$ROOT/etc/init.d/S03sion"
install -D -m 0755 "$P/sion-tools/S41sion-net" "$ROOT/etc/init.d/S41sion-net"
install -D -m 0755 "$P/sion-tools/S99sion-hwlog" "$ROOT/etc/init.d/S99sion-hwlog"
install -D -m 0755 "$P/theseus/theseus-stick" "$ROOT/usr/libexec/theseus-stick"
# Installing onto an internal disk is for the Sion only.
rm -f "$ROOT/usr/sbin/sion-install-internal"
cat > "$ROOT/etc/sion-labels" <<'EOF'
# Generic PC test image: its own partition labels and UEFI entry names, and
# nothing else (a Sion's SIONBOOT/SIONDATA and "Sion" entries are never used).
ESP_LABEL=CRIMSONBOOT
DATA_LABEL=CRIMSONDATA
SION_STRICT_LABELS=1
EFI_ENTRY="Crimson test"
EFI_TRIAL_ENTRY="Crimson test trial"
EOF
echo "crimson-pc-test $(date -u +%Y-%m-%dT%H:%MZ) $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null)" > "$ROOT/etc/sion-version"
echo crimson-test > "$ROOT/etc/hostname"

# ---- firmware and kernel modules ---------------------------------------------
rm -rf "$ROOT/lib/firmware"
cp -a "$FW/lib/firmware" "$ROOT/lib/firmware"
rm -rf "$ROOT/lib/modules"
make -s -C "$LINUX" INSTALL_MOD_PATH="$W/kmod" INSTALL_MOD_STRIP=1 modules_install
cp -a "$W/kmod/lib/modules" "$ROOT/lib/modules"
rm -f "$ROOT"/lib/modules/*/build "$ROOT"/lib/modules/*/source

# ---- initramfs into the kernel ----------------------------------------------
(cd "$ROOT" && find . | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) > "$W/root.cpio"
sed -i 's|^CONFIG_INITRAMFS_SOURCE=.*|CONFIG_INITRAMFS_SOURCE="'"$W/root.cpio"'"|' "$LINUX/.config"
grep -q '^CONFIG_INITRAMFS_COMPRESSION_ZSTD=y' "$LINUX/.config" || {
	sed -i '/^CONFIG_INITRAMFS_COMPRESSION/d;/^# CONFIG_INITRAMFS_COMPRESSION/d' "$LINUX/.config"
	echo 'CONFIG_INITRAMFS_COMPRESSION_ZSTD=y' >> "$LINUX/.config"
}
make -s -C "$LINUX" olddefconfig
make -s -C "$LINUX" -j"$(nproc)" bzImage
cp "$LINUX/arch/x86/boot/bzImage" "$OUT/BOOTX64.EFI"

# ---- the stick: CRIMSONBOOT (FAT, 2 GiB) + CRIMSONDATA (ext4, grows) ---------
ESP_MB=2048 DATA_MB=64
E=$W/esp D=$W/data IMG=$OUT/crimson-pc-test.img
rm -f "$E" "$D" "$IMG"
mkfs.vfat -F 32 -n CRIMSONBOOT -C "$E" $((ESP_MB * 1024)) > /dev/null
mmd -i "$E" ::EFI ::EFI/BOOT ::sion ::sion/logs ::theseus
mcopy -i "$E" "$OUT/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$E" "$EXT/board/common/theseus-README.txt" ::theseus/README.txt
mcopy -i "$E" "$P/sion-tools/README.txt" ::sion/README.txt
mcopy -i "$E" "$HERE/README-pc.txt" ::README.txt
# SION_CONF can be a Sion stick's sion.conf (its Wi-Fi); a fixed address
# there is the Sion's own, so this stick always takes DHCP, under its own name.
tr -d '\r' < "${SION_CONF:-$P/sion-tools/sion.conf}" | sed 's/^HOSTNAME=.*/HOSTNAME="crimson-test"/;
	s/^WIFI_ADDRESS=.*/WIFI_ADDRESS=/; s/^WIFI_GATEWAY=.*/WIFI_GATEWAY=/; s/^WIFI_DNS=.*/WIFI_DNS=/' | sed 's/$/\r/' > "$W/sion.conf"
mcopy -i "$E" "$W/sion.conf" ::sion/sion.conf
if [ -n "$SION_KEYS" ]; then cp "$SION_KEYS" "$W/authorized_keys"; else : > "$W/authorized_keys"; fi
mcopy -i "$E" "$W/authorized_keys" ::sion/authorized_keys
mkfs.ext4 -q -b 4096 -L CRIMSONDATA "$D" ${DATA_MB}M
truncate -s $(( (1 + ESP_MB + DATA_MB + 1) * 1024 * 1024 )) "$IMG"
sfdisk -q "$IMG" <<EOF
label: gpt
start=2048, size=$((ESP_MB * 2048)), type=U, name=boot
start=$((2048 + ESP_MB * 2048)), size=$((DATA_MB * 2048)), type=L, name=data
EOF
dd if="$E" of="$IMG" bs=1M seek=1 conv=notrunc status=none
dd if="$D" of="$IMG" bs=1M seek=$((1 + ESP_MB)) conv=notrunc status=none
ls -la "$OUT/BOOTX64.EFI" "$IMG"
