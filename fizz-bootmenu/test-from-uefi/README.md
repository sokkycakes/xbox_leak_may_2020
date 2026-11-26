# Try the boot menu without flashing

`crimsonboot.efi` is the ROM's LinuxBoot payload (same kernel config, same
menu, same 480i EDID) built as an EFI app with its initramfs and command line
inside. Today's UEFI starts it once, through BootNext.

On the Sion, as root, from this folder: `sh sion-install.sh`, then reboot.
Expect: the firmware splash as now, then the TV switches to 480i and shows
the menu; Crimson boots after 3 s (or pick an entry with a keyboard or pad).
The following boot is the normal one. `--again` arms another test boot,
`--remove` cleans up.

What this tests: i915 at 2880x480i from LinuxBoot, the font on the real TV,
keyboard/pad, finding SIONBOOT on the SSD, and the kexec into Crimson.
What it can't: coreboot's payload switch and its fallback (that's in the
ROM), and "UEFI (edk2)", which here just reboots.

Before the menu, the firmware's own splash is at the adapter's mode as today.
