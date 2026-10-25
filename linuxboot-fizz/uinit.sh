# LinuxBoot uinit for fizz: run the u-root boot menu (scans NVMe/SATA/USB/SD
# for grub.cfg, syslinux and BLS entries and kexecs the chosen kernel).
# If nothing boots, drop to a shell instead of letting init exit (which would
# panic the kernel). Leaving the shell rescans devices.
while true; do
	boot
	echo ""
	echo "No OS was booted. Starting a LinuxBoot shell."
	echo "Type 'exit' to rescan boot devices, or 'shutdown reboot' to restart."
	gosh
done
