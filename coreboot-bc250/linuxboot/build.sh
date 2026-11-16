#!/usr/bin/env bash
# Build a LinuxBoot kernel for the ASRock BC-250 as one EFI file:
# Linux (EFI stub) with a u-root initramfs linked in.
#
# Today: copy it to a USB stick as EFI/BOOT/BOOTX64.EFI and boot it from the
# stock UEFI. No flashing. It finds Bazzite on the NVMe and kexecs into it.
# Later: the same kernel goes into SPI flash in place of the UEFI DXE/BDS
# stage, or as the coreboot payload once the coreboot port can boot Linux.
#
# Requirements: git, go (>= 1.22), gcc, make, flex, bison, bc, libelf-dev,
# libssl-dev, xz-utils, cpio.
#
# Usage: ./build.sh [workdir]   (default workdir: ./work)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mkdir -p "${1:-$HERE/work}" && cd "${1:-$HERE/work}" && pwd)"
KERNEL_TAG="${KERNEL_TAG:-v6.18.54}"
UROOT_TAG="${UROOT_TAG:-v0.16.0}"
JOBS="${JOBS:-$(nproc)}"
cd "$WORK"

[ -d linux ] || git clone --depth 1 -b "$KERNEL_TAG" \
	https://github.com/gregkh/linux.git linux
[ -d u-root ] || git clone --depth 1 -b "$UROOT_TAG" \
	https://github.com/u-root/u-root.git u-root

# --- u-root initramfs (built first: the kernel links it in) -----------------
(cd u-root && go build -o u-root . && GOARCH=amd64 ./u-root \
	-build=bb \
	-initcmd=init \
	-uinitcmd="gosh /etc/linuxboot-uinit.sh" \
	-defaultsh=gosh \
	-files "$HERE/uinit.sh:etc/linuxboot-uinit.sh" \
	-o "$WORK/initramfs.cpio" \
	core ./cmds/boot/boot ./cmds/exp/acpicat ./cmds/exp/dmidecode)

# --- Kernel ------------------------------------------------------------------
sed "s|@INITRAMFS@|$WORK/initramfs.cpio|" "$HERE/kernel.config.fragment" \
	> "$WORK/kernel.config.fragment"
make -C linux allnoconfig
(cd linux && scripts/kconfig/merge_config.sh -m .config \
	"$WORK/kernel.config.fragment")
make -C linux olddefconfig
# Fail if a requested option did not stick (missing dependency, renamed).
(cd linux && grep -E '^CONFIG_[A-Z0-9_]+=' "$WORK/kernel.config.fragment" |
	grep -v -e '=n$' -e INITRAMFS_SOURCE | while read -r l; do
		grep -qxF "$l" .config || echo "not set: $l"; done) | tee "$WORK/config-missing.txt"
make -C linux -j"$JOBS" bzImage \
	KBUILD_BUILD_USER=linuxboot KBUILD_BUILD_HOST=bc250
cp linux/arch/x86/boot/bzImage "$WORK/linuxboot-bc250.efi"
(cd "$WORK" && sha256sum linuxboot-bc250.efi > linuxboot-bc250.efi.sha256)
ls -l "$WORK/linuxboot-bc250.efi"
