#!/usr/bin/env bash
# Read-only hardware survey of a running BC-250, for the coreboot port.
#
# It only READS: PCI config space, ACPI tables, kernel logs, GPIO/pinmux
# state from debugfs, SMBIOS, DisplayPort link state and EDIDs. It never writes to the SPI flash, PCI devices
# or Super I/O, and needs no extra packages (missing tools are skipped).
#
# Usage:  sudo ./bc250-collect.sh
# Output: bc250-info-<date>.tar.gz in the current directory.
# It may contain serial numbers (SMBIOS, NIC MAC); look before sharing.
set -u

OUT="bc250-info-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
cd "$OUT" || exit 1

run() { # run NAME CMD...  -> NAME.txt, never fails
	local name=$1; shift
	if command -v "$1" >/dev/null 2>&1; then
		"$@" > "$name.txt" 2>&1 || true
	else
		echo "$1 not installed" > "$name.txt"
	fi
}

[ "$(id -u)" -eq 0 ] || echo "warning: not root; some dumps will be incomplete" | tee NOT_ROOT.txt

run uname        uname -a
cp /etc/os-release os-release.txt 2>/dev/null
run cpuinfo      cat /proc/cpuinfo
run cmdline      cat /proc/cmdline
run dmesg        dmesg
run lspci-tree   lspci -tvnn
run lspci-full   lspci -nnvvvxxxx
run lsusb        lsusb -tv
run lsusb-full   lsusb -v
run dmidecode    dmidecode
run iomem        cat /proc/iomem
run ioports      cat /proc/ioports
run interrupts   cat /proc/interrupts
run tty          sh -c 'ls -l /sys/class/tty/ | grep -v virtual; cat /proc/tty/driver/serial'
run modules      lsmod
run sensors      sensors

# ACPI tables: raw binaries (DSDT/SSDT give IRQ routing, GPIO use, UARTs)
mkdir -p acpi
for t in /sys/firmware/acpi/tables/* /sys/firmware/acpi/tables/dynamic/*; do
	[ -f "$t" ] && cp "$t" "acpi/$(basename "$t")" 2>/dev/null
done

# AMD GPIO / pin mux state (pinctrl-amd)
mount -t debugfs none /sys/kernel/debug 2>/dev/null
run gpio-debugfs cat /sys/kernel/debug/gpio
for d in /sys/kernel/debug/pinctrl/*; do
	[ -d "$d" ] || continue
	n=$(basename "$d")
	for f in pins pinmux-pins pinconf-pins gpio-ranges; do
		[ -f "$d/$f" ] && cat "$d/$f" > "pinctrl-$n-$f.txt" 2>/dev/null
	done
done

# GPU firmware and VBIOS info
for d in /sys/kernel/debug/dri/*; do
	[ -f "$d/amdgpu_firmware_info" ] && cat "$d/amdgpu_firmware_info" > "amdgpu-fw-$(basename "$d").txt" 2>/dev/null
	[ -f "$d/amdgpu_vbios" ] && cp "$d/amdgpu_vbios" "amdgpu-vbios-$(basename "$d").bin" 2>/dev/null
done

# DisplayPort state, for the flaky-DP reports: link rate/lanes the driver
# trained to versus what the sink reports, PHY swing/pre-emphasis, EDIDs.
for c in /sys/class/drm/card*-*; do
	[ -d "$c" ] || continue
	n=$(basename "$c")
	{
		echo "status: $(cat "$c/status" 2>/dev/null)"
		echo "enabled: $(cat "$c/enabled" 2>/dev/null)"
		echo "modes:"; cat "$c/modes" 2>/dev/null
	} > "drm-$n.txt"
	[ -s "$c/edid" ] && cp "$c/edid" "drm-$n.edid" 2>/dev/null
done
for d in /sys/kernel/debug/dri/*/DP-* /sys/kernel/debug/dri/*/HDMI-A-*; do
	[ -d "$d" ] || continue
	n=$(echo "$d" | sed 's#/sys/kernel/debug/dri/##; s#/#-#g')
	for f in link_settings phy_settings output_bpc dp_link_rate hdcp_sink_capability; do
		[ -r "$d/$f" ] && { echo "== $f"; cat "$d/$f"; } >> "dp-$n.txt" 2>/dev/null
	done
done
run dmesg-drm    sh -c 'dmesg | grep -iE "drm|amdgpu|dp |displayport|link train|hpd|aux"'

# EFI variables list (names only) and boot info
run efivars ls /sys/firmware/efi/efivars
run efibootmgr efibootmgr -v

cd ..
tar czf "$OUT.tar.gz" "$OUT" && rm -rf "$OUT"
echo "wrote $PWD/$OUT.tar.gz"
