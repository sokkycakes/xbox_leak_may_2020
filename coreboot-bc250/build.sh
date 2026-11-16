#!/usr/bin/env bash
# Build the proof-of-life coreboot image for the ASRock BC-250 (AMD Cyan
# Skillfish) on top of a pinned upstream coreboot commit.
#
# Every AMD blob in the image (PSP firmware, ABL/memory training, SMU, APCB)
# is copied out of a stock BC-250 BIOS; coreboot ships none for this SoC.
#
# Requirements: git, python3, make, gcc >= 14 (amdfwtool needs -std=gnu23),
# flex, bison, iasl (acpica-tools), zlib/ncurses headers, curl.
#
# Usage: ./build.sh [halt|resetloop|continue] [STOCK.ROM]
#   halt       print over serial, then halt (default)
#   resetloop  print, then reset every 5 s: visible without serial
#   continue   carry on into romstage/ramstage (porting work; will fail)
# STOCK.ROM defaults to the public P3.00 dump, downloaded and hash-checked.
#
# Env: WORK (default ./work), JOBS, XGCCPATH (reuse a built crossgcc),
#      HOSTCC (default gcc-14 if present).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VARIANT="${1:-halt}"
WORK="$(mkdir -p "${WORK:-$HERE/work}" && cd "${WORK:-$HERE/work}" && pwd)"
JOBS="${JOBS:-$(nproc)}"
COREBOOT_COMMIT="f13a6c7a61a761559d719f2b2e2a0ae1f43ddeb9"
STOCK_URL="https://gitlab.com/TuxThePenguin0/bc250-bios/-/raw/main/BC250_3.00.ROM"
STOCK_SHA256="07595ca3aecf8a4caa28a397b5298f3946a1b769f87b16f67adc369c3f69045c"
HOSTCC="${HOSTCC:-$(command -v gcc-14 || echo gcc)}"
HOSTCXX="${HOSTCXX:-$(command -v g++-14 || echo g++)}"

case "$VARIANT" in
	halt|resetloop|continue) ;;
	*) echo "unknown variant '$VARIANT'" >&2; exit 1 ;;
esac

cd "$WORK"

# --- Stock BIOS ----------------------------------------------------------------
if [ -n "${2:-}" ]; then
	STOCK="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
else
	STOCK="$WORK/BC250_3.00.ROM"
	[ -f "$STOCK" ] || curl -fL -o "$STOCK" "$STOCK_URL"
	echo "$STOCK_SHA256  $STOCK" | sha256sum -c -
fi

# --- coreboot ------------------------------------------------------------------
if [ ! -d coreboot ]; then
	git init -q coreboot
	git -C coreboot remote add origin https://github.com/coreboot/coreboot.git
	git -C coreboot fetch -q --depth 1 origin "$COREBOOT_COMMIT"
	git -C coreboot checkout -q FETCH_HEAD
	git -C coreboot -c user.name=build -c user.email=build@localhost \
		am -q "$HERE"/patches/*.patch
fi
cd coreboot

if [ -z "${XGCCPATH:-}" ] && [ ! -x util/crossgcc/xgcc/bin/i386-elf-gcc ]; then
	make crossgcc-i386 CPUS="$JOBS"
fi

# --- Blobs ---------------------------------------------------------------------
python3 -I "$HERE/extract_stock_blobs.py" "$STOCK" 3rdparty/bc250_blobs

# --- Build ---------------------------------------------------------------------
{
	echo "CONFIG_VENDOR_ASROCK=y"
	echo "CONFIG_BOARD_ASROCK_BC250=y"
	echo "CONFIG_PAYLOAD_NONE=y"
	case "$VARIANT" in
		halt)      echo "CONFIG_BC250_PROOF_OF_LIFE_HALT=y" ;;
		resetloop) echo "CONFIG_BC250_PROOF_OF_LIFE_RESET_LOOP=y" ;;
		continue)  echo "CONFIG_BC250_PROOF_OF_LIFE_CONTINUE=y" ;;
	esac
} > .config
make olddefconfig HOSTCC="$HOSTCC" HOSTCXX="$HOSTCXX"
make -j"$JOBS" HOSTCC="$HOSTCC" HOSTCXX="$HOSTCXX"

OUT="$WORK/coreboot-bc250-$VARIANT.rom"
cp build/coreboot.rom "$OUT"
python3 -I "$HERE/compare_psp_layout.py" "$STOCK" "$OUT"
sha256sum "$OUT"
