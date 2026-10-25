# MrChromebox fizz firmware with a LinuxBoot payload

This directory rebuilds MrChromebox's coreboot firmware for **Google Fizz**
with **LinuxBoot** (a Linux kernel plus a u-root initramfs) as the payload,
in place of edk2/UEFI. One ROM covers every fizz variant:

| Board   | Device                     |
|---------|----------------------------|
| SION    | Acer Chromebox CXI3        |
| TEEMO   | ASUS Chromebox 3 (CN65)    |
| WUKONG  | CTL Chromebox CBx1         |
| KENCH   | HP Chromebox G2            |
| JAX     | AOpen Chromebox Commercial 2 |

## What's in the ROM

- **coreboot**: MrChromebox `MrChromebox-2609.0`, board `google/fizz`. It uses
  the same IFD, ME, EC-RW and FSP blobs as the stock MrChromebox UEFI build
  (`configs/kbl/config.fizz.uefi`).
- **Graphics**: coreboot's native **libgfxinit** sets up a linear framebuffer.
  The edk2 build uses Intel's GOP driver, which only works under UEFI. The
  kernel shows the framebuffer through simpledrm, then hands off to `i915`.
- **Kernel**: Linux `6.18.54` (LTS), built from `allnoconfig` plus
  `kernel.config.fragment`. Every driver is built in: NVMe, AHCI, xHCI, USB
  storage and HID, Realtek SD reader, RTL8111 (r8169), i915, ext4, btrfs,
  xfs, f2fs, vfat, exfat, ntfs3, iso9660, dm-crypt, cros_ec, the coreboot
  tables and the Cr50 TPM.
- **Initramfs**: u-root `v0.16.0` (`core` commands plus `boot`, `pxeboot` and
  `cbmem`), xz-compressed.
- **Boot flow**: `uinit.sh` runs u-root's `boot`. It scans every block device
  for GRUB (`grub.cfg`), syslinux and BootLoaderSpec entries, shows a menu,
  and kexecs the entry you pick (or the default entry after a timeout). If
  nothing boots, you get a `gosh` shell. Type `exit` to rescan.

Flash layout: 16 MiB total. The COREBOOT region is 13.9 MiB, and the payload
takes about 10 MiB of it, leaving about 2.7 MiB free.

## Building

```sh
./build.sh            # outputs work/coreboot_linuxboot-fizz-mrchromebox_<date>.rom
```

You need git, Go 1.22 or newer, Docker, and the usual kernel build
dependencies. coreboot builds inside the `coreboot/coreboot-sdk` image, so
you don't have to build crossgcc yourself. Override the versions with
`COREBOOT_TAG`, `KERNEL_TAG`, `UROOT_TAG` or `SDK_IMAGE` if needed.

## Flashing

Flash it like any MrChromebox full ROM: firmware write protect must be
**disabled** (on fizz, remove the WP screw or use CCD). **Back up your
current firmware first.**

```sh
# Back up the existing firmware
sudo flashrom -p internal -r backup.rom
# Carry over the VPD so the Ethernet MAC address and serial number survive
cbfstool backup.rom read -r RO_VPD -f vpd.bin
cbfstool coreboot_linuxboot-fizz-*.rom write -r RO_VPD -f vpd.bin
# Flash (the descriptor and ME regions are left alone)
sudo flashrom -p internal --ifd -i bios -w coreboot_linuxboot-fizz-*.rom
```

Keep a way to recover before you flash (a SuzyQ/CCD cable or an external
SPI programmer, plus your backup). This build has not been tested on real
hardware yet.

## Notes and limitations

- **No UEFI.** LinuxBoot boots Linux distributions by kexecing the kernel
  and initrd named in their `grub.cfg` or BLS entries. It **cannot boot
  Windows** or anything else that needs UEFI services. Encrypted `/boot`
  and some complex GRUB scripts may not parse.
- **Console** is `tty0` (HDMI/DisplayPort). Fizz has no external serial
  port. Add `console=ttyS0,115200` to `CONFIG_LINUX_COMMAND_LINE` in
  `config.fizz.linuxboot` if you use a SuzyQ cable.
- No firmware files (i915 DMC/GuC, Wi-Fi) are embedded. i915 modesetting
  works without them, and the installed OS loads its own firmware after
  kexec.
