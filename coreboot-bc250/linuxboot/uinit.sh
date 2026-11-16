# LinuxBoot uinit for the BC-250. Print what the firmware handed us, then run
# the u-root boot menu: it scans NVMe/SATA/USB for BLS entries (Bazzite),
# grub.cfg and syslinux, and kexecs the chosen kernel. If nothing boots, drop
# to a shell; leaving the shell rescans.
echo ""
echo "=== LinuxBoot on BC-250 ==="
cat /proc/version
grep -m1 "model name" /proc/cpuinfo
grep MemTotal /proc/meminfo
echo "=== PCI devices ==="
pci
echo ""
while true; do
	boot
	echo ""
	echo "No OS was booted. Starting a LinuxBoot shell."
	echo "Type 'exit' to rescan boot devices, or 'shutdown reboot' to restart."
	gosh
done
