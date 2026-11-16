# LinuxBoot for the BC-250, booted from USB (no flashing)

`linuxboot-bc250.efi` is a Linux kernel with a u-root initramfs linked in,
built as an EFI application. The stock ASRock UEFI starts it from a USB
stick, it prints what it sees, scans the disks for Bazzite's boot entries
(BLS), and kexecs into Bazzite. Nothing on the SPI chip changes.

This tests everything about LinuxBoot except where it lives: the kernel
config, the drivers for this board, the boot menu, and the kexec handoff
into the real OS. Moving it into flash later (in place of the UEFI's
DXE/boot stage, or as a coreboot payload) is a separate, flashing step.

**Status: boots and kexecs in QEMU with OVMF. Not yet run on a BC-250.**

## Try it

1. Format a USB stick as FAT32 (a GPT or MBR partition is fine).
2. Copy `linuxboot-bc250.efi` to `EFI/BOOT/BOOTX64.EFI` on it.
3. Boot the BC-250 and pick the stick from the boot menu (F11 on the
   stock BIOS) or move it to the top of the boot order. Secure Boot is not
   present in the stock image, so the unsigned file runs.
4. You should see `=== LinuxBoot on BC-250 ===`, the CPU name, memory,
   the PCI device list, then `Welcome to LinuxBoot's Menu` with your
   Bazzite entries. It boots the first one after a short timeout.
5. If something fails you get a `gosh` shell. Useful commands there:
   `dmesg`, `pci`, `ls /dev`, `cat /proc/cmdline`, `boot` (retry).

What to report back: whether the screen shows the console at all (it uses
the stock GOP framebuffer), whether NVMe, USB keyboard and the RTL8111H
(`ip link`) show up, and whether Bazzite boots and gets working graphics
after the kexec.

## Rebuilding

```sh
./build.sh            # work dir defaults to ./work
```

Fetches Linux `v6.18.54` and u-root `v0.16.0`, builds u-root, links it into
the kernel, and checks every requested kernel option actually got set.

## Notes

- No amdgpu in this kernel. It would need the Cyan Skillfish firmware
  linked in, and LinuxBoot only needs a text console. Bazzite's own kernel
  brings the GPU up after kexec.
- The IOMMU driver is left out, since the BC-250 needs it off.
- Console goes to the screen and to Super I/O COM1 (0x3F8, 115200 8N1).
- Test in QEMU: put the file on a FAT directory as `EFI/BOOT/BOOTX64.EFI`
  and run `qemu-system-x86_64 -m 2048 -nographic -drive
  if=pflash,format=raw,readonly=on,file=OVMF_CODE_4M.fd -drive
  if=pflash,format=raw,file=OVMF_VARS_4M.fd -drive format=raw,file=fat:rw:esp`.
