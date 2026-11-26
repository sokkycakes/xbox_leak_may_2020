#!/usr/bin/env bash
# Build MrChromebox's coreboot for Google Fizz (the Sion) with two payloads
# and a 15 kHz boot menu:
#
#   img/linuxboot     the default: Linux + u-root running crimsonboot, a menu
#                     on the TV at 2880x480i that kexecs Crimson directly
#   fallback/payload  MrChromebox's edk2 (UEFI), unchanged; crimsonboot's
#                     "UEFI (edk2)" entry reboots into it once
#
# coreboot picks between them with one CMOS byte (coreboot-patches/0001).
#
# Requirements: git, go (>= 1.22), docker, plus the usual kernel build deps
# (gcc, make, flex, bison, bc, libelf-dev, libssl-dev, xz-utils, cpio).
# coreboot and edk2 build inside the coreboot-sdk Docker image.
#
# Usage: ./build.sh [workdir]   (default workdir: ./work)
#
#   VIDEO=480i     (default) LinuxBoot loads crimson-480i.edid for CONNECTOR,
#                  so the menu and the handoff are 15 kHz 2880x480i
#   VIDEO=native   no EDID override: the display's own preferred mode
#   CONNECTOR=HDMI-A-1   the port the transcoder hangs off
#   TIMEOUT=3      seconds before Crimson boots by itself (menu shown)
#   SHOW=hold      (default) no menu unless a button is held at power-on:
#                  the power button, a key, or a gamepad button
#   SHOW=always    the menu every boot, with the TIMEOUT countdown
#   HOLDWAIT=2500  ms after kernel start to look for a held button
#   BOOTANI=1      (default) with SHOW=hold, the boot animation plays from
#                  the firmware while Crimson loads, and its last frame stays
#                  up through the handoff. Builds a trimmed Intel Mesa (iris
#                  for Kaby Lake, no LLVM) and bootani on the host: needs
#                  meson, python3-mako, cmake, libdrm-dev and zlib1g-dev.
#                  BOOTANI=0 leaves it out (the screen stays black).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mkdir -p "${1:-$HERE/work}" && cd "${1:-$HERE/work}" && pwd)"

COREBOOT_TAG="${COREBOOT_TAG:-MrChromebox-2609.0}"
KERNEL_TAG="${KERNEL_TAG:-v6.18.54}"
UROOT_TAG="${UROOT_TAG:-v0.16.0}"
SDK_IMAGE="${SDK_IMAGE:-coreboot/coreboot-sdk:2026-07-06_c165edb4ce}"
JOBS="${JOBS:-$(nproc)}"
VIDEO="${VIDEO:-480i}"
CONNECTOR="${CONNECTOR:-HDMI-A-1}"
TIMEOUT="${TIMEOUT:-3}"
SHOW="${SHOW:-hold}"
HOLDWAIT="${HOLDWAIT:-2500}"
BOOTANI="${BOOTANI:-1}"
[ "$SHOW" = hold ] || BOOTANI=0
MESA_VERSION="${MESA_VERSION:-24.0.9}"

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

git -C coreboot config url."https://github.com/coreboot/".insteadOf \
	"https://review.coreboot.org/"
for sm in blobs fsp intel-microcode vboot libgfxinit libhwbase; do
	git -C coreboot submodule update --init --checkout "3rdparty/$sm"
done
git -C coreboot/3rdparty/libgfxinit submodule update --init
if [ ! -d coreboot/src/drivers/payload_select ]; then
	git -C coreboot apply "$HERE"/coreboot-patches/*.patch
fi

# --- Kernel ------------------------------------------------------------------
if ! grep -q kexec_keep_display linux/drivers/gpu/drm/i915/i915_driver.c; then
	git -C linux apply "$HERE"/linux-patches/*.patch
fi
sed "s|@FIRMWARE_DIR@|$HERE/firmware|" "$HERE/kernel.config.fragment" \
	> "$WORK/kernel.config.fragment"
make -C linux allnoconfig
(cd linux && scripts/kconfig/merge_config.sh -m .config \
	"$WORK/kernel.config.fragment")
make -C linux olddefconfig
(cd linux && grep -E '^CONFIG_[A-Z0-9_]+=' "$WORK/kernel.config.fragment" |
	grep -v '=n$' | while read -r l; do
		grep -qxF "$l" .config || echo "not set: $l"; done) | tee "$WORK/config-missing.txt"
[ ! -s "$WORK/config-missing.txt" ] || { echo "kernel options missing" >&2; exit 1; }
make -C linux -j"$JOBS" bzImage \
	KBUILD_BUILD_USER=linuxboot KBUILD_BUILD_HOST=fizz
cp linux/arch/x86/boot/bzImage "$PAYLOAD/bzImage"

# --- bootani: trimmed Mesa + the animation, for the initramfs ----------------
UROOT_FILES=()
if [ "$BOOTANI" = 1 ]; then
	if [ ! -d "mesa-$MESA_VERSION" ]; then
		curl -fsSL "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz" | tar -xJ
		for p in "$HERE"/mesa-patches/*.patch; do patch -d "mesa-$MESA_VERSION" -p1 < "$p"; done
	fi
	MESA="$WORK/mesa-stage"
	if [ ! -f "$MESA/usr/lib/dri/iris_dri.so" ]; then
		(cd "mesa-$MESA_VERSION" && rm -rf build &&
		meson setup build --prefix=/usr --libdir=lib -Dbuildtype=minsize \
			-Db_ndebug=true -Db_lto=true -Dstrip=true \
			-Dc_args="-ffunction-sections -fdata-sections" \
			-Dcpp_args="-ffunction-sections -fdata-sections" \
			-Dc_link_args="-Wl,--gc-sections" \
			-Dcpp_link_args="-static-libstdc++ -static-libgcc -Wl,--gc-sections" \
			-Dgallium-drivers=iris -Dvulkan-drivers= -Dplatforms= \
			-Degl=enabled -Dgbm=enabled -Dopengl=true -Dgles1=disabled \
			-Dgles2=disabled -Dglx=disabled -Dllvm=disabled -Dshared-glapi=enabled \
			-Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled \
			-Dgallium-nine=false -Dgallium-opencl=disabled -Dgallium-rusticl=false \
			-Dvideo-codecs= -Dtools= -Dshader-cache=disabled -Dzstd=disabled \
			-Dexpat=disabled -Dxmlconfig=disabled -Dlibunwind=disabled \
			-Dvalgrind=disabled -Dintel-clc=disabled -Dglvnd=false \
			-Dlmsensors=disabled -Dbuild-tests=false &&
		ninja -C build && rm -rf "$MESA" && DESTDIR="$MESA" ninja -C build install)
	fi
	rm -rf bootani-build
	PKG_CONFIG_PATH="$MESA/usr/lib/pkgconfig" cmake -S "$HERE/../bootani" -B bootani-build \
		-DCMAKE_BUILD_TYPE=MinSizeRel -DBOOTANI_WITH_SDL2=OFF -DBOOTANI_WITH_EGL=ON \
		-DBOOTANI_WITH_KMS=ON -DBOOTANI_ALSA_RAW=ON -DCMAKE_PREFIX_PATH="$MESA/usr" \
		-DCMAKE_C_FLAGS="-I$MESA/usr/include" -DCMAKE_CXX_FLAGS="-I$MESA/usr/include" \
		-DCMAKE_EXE_LINKER_FLAGS="-L$MESA/usr/lib -Wl,-rpath-link,$MESA/usr/lib -static-libstdc++ -static-libgcc -s"
	cmake --build bootani-build -j"$JOBS"
	# bootani and Mesa in /usr/lib, the host's glibc, libdrm and zlib where
	# its loader looks (u-root also adds bootani's own libraries there).
	BR="$WORK/bootani-root"
	rm -rf "$BR" && mkdir -p "$BR/usr/lib/dri" "$BR/lib/x86_64-linux-gnu" "$BR/lib64"
	cp bootani-build/bootani "$BR/bootani"
	cp "$MESA/usr/lib/dri/iris_dri.so" "$BR/usr/lib/dri/"
	for l in libEGL.so.1 libgbm.so.1 libglapi.so.0; do cp -L "$MESA/usr/lib/$l" "$BR/usr/lib/"; done
	for l in libdrm.so.2 libz.so.1 libc.so.6 libm.so.6; do
		cp -L "/lib/x86_64-linux-gnu/$l" "$BR/lib/x86_64-linux-gnu/"
	done
	cp -L /lib64/ld-linux-x86-64.so.2 "$BR/lib64/"
	strip -s "$BR"/usr/lib/*.so* "$BR"/usr/lib/dri/*.so 2>/dev/null || true
	UROOT_FILES+=(-files "$BR/bootani:usr/bin/bootani" -files "$BR/lib64/ld-linux-x86-64.so.2:lib64/ld-linux-x86-64.so.2")
	for f in "$BR"/lib/x86_64-linux-gnu/* "$BR"/usr/lib/*.so* "$BR"/usr/lib/dri/*.so; do
		UROOT_FILES+=(-files "$f:${f#"$BR"/}")
	done
fi

# --- u-root initramfs with the menu ------------------------------------------
rm -rf u-root/cmds/exp/crimsonboot
cp -r "$HERE/crimsonboot" u-root/cmds/exp/crimsonboot
(cd u-root && go build -o u-root . && GOARCH=amd64 ./u-root \
	-build=bb \
	-initcmd=crimsonboot \
	-defaultsh=gosh \
	"${UROOT_FILES[@]}" \
	-o "$WORK/initramfs.cpio" \
	./cmds/exp/crimsonboot \
	./cmds/core/{gosh,ls,cat,cp,mkdir,mount,umount,dmesg,grep,ps,kexec,shutdown,sync,pci,hexdump,io,echo,sleep,uname})
xz --stdout --force --check=crc32 --lzma2=preset=9e,dict=16MiB "$WORK/initramfs.cpio" \
	> "$PAYLOAD/initramfs.cpio.xz"

CMDLINE="console=tty0 quiet loglevel=1 vt.global_cursor_default=0 i915.fastboot=1 crimsonboot.timeout=$TIMEOUT"
CMDLINE="$CMDLINE crimsonboot.cmos=$(sed -n 's/^CONFIG_PAYLOAD_CMOS_SELECT_OFFSET=//p' "$HERE/config.fizz.bootmenu")"
case "$SHOW" in
hold) CMDLINE="$CMDLINE crimsonboot.show=hold crimsonboot.holdwait=$HOLDWAIT"
	[ "$BOOTANI" = 1 ] && CMDLINE="$CMDLINE i915.kexec_keep_display=1" ;;
always) ;;
*) echo "SHOW must be hold or always" >&2; exit 1 ;;
esac
case "$VIDEO" in
480i) CMDLINE="$CMDLINE drm.edid_firmware=$CONNECTOR:edid/crimson-480i.edid" ;;
native) ;;
*) echo "VIDEO must be 480i or native" >&2; exit 1 ;;
esac
echo "$CMDLINE" > "$PAYLOAD/cmdline.txt"

# --- coreboot + edk2 ---------------------------------------------------------
cd coreboot
rm -rf build
cp "$HERE/config.fizz.bootmenu" .config
echo "CONFIG_LOCALVERSION=\"$(git describe --tags)-bootmenu\"" >> .config

# edk2 and iPXE are cloned inside the container. Behind a proxy, pass it in,
# and SDK_CA=/path/to/ca.crt if the proxy re-signs TLS.
SDK_NET=()
[ -z "${HTTPS_PROXY:-}" ] || SDK_NET+=(--network host -e HTTPS_PROXY -e HTTP_PROXY -e https_proxy="$HTTPS_PROXY" -e http_proxy="${HTTP_PROXY:-}" -e no_proxy)
[ -z "${SDK_CA:-}" ] || SDK_NET+=(-v "$SDK_CA:/ca.crt:ro" -e GIT_SSL_CAINFO=/ca.crt -e SSL_CERT_FILE=/ca.crt -e CURL_CA_BUNDLE=/ca.crt)
sdk() {
	docker run --rm -u root -e HOME=/root "${SDK_NET[@]}" -v "$WORK:$WORK" -v "$HERE:$HERE" -w "$PWD" \
		"$SDK_IMAGE" sh -c 'git config --global --add safe.directory "*"; exec "$@"' sh "$@"
}
sdk make olddefconfig
grep -qx 'CONFIG_PAYLOAD_CMOS_SELECT=y' .config || { echo "payload_select is not in the config" >&2; exit 1; }
sdk make -j"$JOBS"

# LinuxBoot next to edk2. The kernel is xz already and so is the initramfs.
sdk build/cbfstool build/coreboot.rom add-payload -r COREBOOT -n img/linuxboot \
	-f "$PAYLOAD/bzImage" -I "$PAYLOAD/initramfs.cpio.xz" -C "$CMDLINE" -c none
sdk build/cbfstool build/coreboot.rom print -r COREBOOT | tee "$WORK/cbfs.txt"

ROM="coreboot_bootmenu-fizz-mrchromebox_$(date +%Y%m%d).rom"
cp build/coreboot.rom "$WORK/$ROM"
(cd "$WORK" && sha256sum "$ROM" > "$ROM.sha256")
echo "Built $WORK/$ROM"
