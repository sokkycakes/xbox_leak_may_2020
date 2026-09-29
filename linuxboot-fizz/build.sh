#!/usr/bin/env bash
# Build MrChromebox's coreboot for Google Fizz (SION/TEEMO/WUKONG/KENCH/JAX)
# with a LinuxBoot payload (Linux + u-root) in place of edk2.
#
# Requirements: git, go (>= 1.22), docker, plus the usual kernel build deps
# (gcc, make, flex, bison, bc, libelf-dev, libssl-dev, xz-utils, cpio).
# coreboot itself is built inside the coreboot-sdk Docker image, so no
# crossgcc build is needed.
#
# Usage: ./build.sh [workdir]   (default workdir: ./work)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mkdir -p "${1:-$HERE/work}" && cd "${1:-$HERE/work}" && pwd)"

COREBOOT_TAG="${COREBOOT_TAG:-MrChromebox-2609.0}"
KERNEL_TAG="${KERNEL_TAG:-v6.18.54}"
UROOT_TAG="${UROOT_TAG:-v0.16.0}"
SDK_IMAGE="${SDK_IMAGE:-coreboot/coreboot-sdk:2026-07-06_c165edb4ce}"
JOBS="${JOBS:-$(nproc)}"

PAYLOAD="$WORK/payload"
mkdir -p "$PAYLOAD"
cd "$WORK"

# --- Sources -----------------------------------------------------------------
[ -d coreboot ] || git clone --depth 1 -b "$COREBOOT_TAG" \
	https://github.com/MrChromebox/coreboot.git coreboot
[ -d linux ] || git clone --depth 1 -b "$KERNEL_TAG" \
	https://github.com/gregkh/linux.git linux
[ -d u-root ] || git clone --depth 1 -b "$UROOT_TAG" \
	https://github.com/u-root/u-root.git u-root

# Only the submodules fizz needs. review.coreboot.org is mirrored on GitHub;
# the insteadOf rewrite is scoped to this repo so it does not leak globally.
git -C coreboot config url."https://github.com/coreboot/".insteadOf \
	"https://review.coreboot.org/"
for sm in blobs fsp intel-microcode vboot libgfxinit libhwbase; do
	git -C coreboot submodule update --init --checkout "3rdparty/$sm"
done
git -C coreboot/3rdparty/libgfxinit submodule update --init

# --- Kernel ------------------------------------------------------------------
make -C linux allnoconfig
(cd linux && scripts/kconfig/merge_config.sh -m .config \
	"$HERE/kernel.config.fragment")
make -C linux olddefconfig
make -C linux -j"$JOBS" bzImage \
	KBUILD_BUILD_USER=linuxboot KBUILD_BUILD_HOST=fizz
cp linux/arch/x86/boot/bzImage "$PAYLOAD/bzImage"

# --- u-root initramfs --------------------------------------------------------
(cd u-root && go build -o u-root . && GOARCH=amd64 ./u-root \
	-build=bb \
	-initcmd=init \
	-uinitcmd="gosh /etc/linuxboot-uinit.sh" \
	-defaultsh=gosh \
	-files "$HERE/uinit.sh:etc/linuxboot-uinit.sh" \
	-o "$WORK/initramfs.cpio" \
	core ./cmds/boot/boot ./cmds/boot/pxeboot ./cmds/exp/cbmem)
xz --stdout --force --check=crc32 --lzma2=dict=1MiB "$WORK/initramfs.cpio" \
	> "$PAYLOAD/initramfs.cpio.xz"

# --- coreboot ----------------------------------------------------------------
cd coreboot
rm -rf build
# The container sees $WORK at the same path, so absolute payload paths work.
sed "s|@PAYLOAD_DIR@|$PAYLOAD|" "$HERE/config.fizz.linuxboot" > .config
echo "CONFIG_LOCALVERSION=\"$(git describe --tags)-linuxboot\"" >> .config

sdk() {
	docker run --rm -u root -e HOME=/root -v "$WORK:$WORK" -w "$PWD" \
		"$SDK_IMAGE" sh -c 'git config --global --add safe.directory "*"; exec "$@"' sh "$@"
}
sdk make olddefconfig
sdk make -j"$JOBS"

ROM="coreboot_linuxboot-fizz-mrchromebox_$(date +%Y%m%d).rom"
cp build/coreboot.rom "$WORK/$ROM"
(cd "$WORK" && sha256sum "$ROM" > "$ROM.sha256")
echo "Built $WORK/$ROM"
