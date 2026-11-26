# Sion boot menu: Crimson at 15 kHz, or UEFI

A MrChromebox coreboot ROM for Google Fizz (the Sion, Acer Chromebox CXI3)
with **two payloads** and a boot menu that shows up on the TV through the
VGA-to-composite transcoder:

| CBFS file          | What it is                                                  |
|--------------------|-------------------------------------------------------------|
| `img/linuxboot`    | **Default.** Linux + u-root running `crimsonboot`, a menu at 2880x480i (15.734 kHz, VIC 10) that kexecs Crimson directly |
| `fallback/payload` | MrChromebox's edk2 (UEFI), exactly as in their stock fizz ROM |

```
power on ─ coreboot ─┬─ CMOS byte says "UEFI once"? ── edk2 (UEFI) ── its boot order (the "Sion" entry, USB, Windows…)
                     └─ otherwise ── LinuxBoot ── i915 lights the TV at 480i ── menu
                                                   ├─ Crimson                  kexec EFI/BOOT/BOOTX64.EFI  (default, 3 s)
                                                   ├─ Crimson, previous image  kexec EFI/sion/previous.efi
                                                   ├─ UEFI (edk2)              set the byte, reboot
                                                   ├─ LinuxBoot shell
                                                   └─ Reboot / Power off
```

## Why this shape

- **Only something running Linux can draw at 15 kHz.** The firmware's own
  graphics (edk2's GOP, libgfxinit) can't do interlaced modes, so any menu
  drawn by firmware (edk2's boot manager, SeaBIOS, GRUB) would be a 31 kHz
  picture the transcoder can't show. The menu therefore lives in LinuxBoot,
  whose kernel has `crimson-480i.edid` built in
  (`drm.edid_firmware=HDMI-A-1:…`), the same EDID Crimson itself uses.
- **No 31 kHz frame on the LinuxBoot path.** The ROM keeps `NO_GFX_INIT`
  like MrChromebox's UEFI build, so coreboot never lights the screen; the
  first frame anyone sees is i915's, already 480i. On the UEFI path edk2's
  Intel GOP lights it at the adapter's own mode, as today.
- **edk2 stays untouched** (the build's own payload, with SMMSTORE, iPXE and
  the GOP driver), so UEFI behaves exactly like the ROM the Sion runs now.
- **The handoff stays 15 kHz.** kexec keeps the display running and Crimson's
  i915 (`fastboot`) takes over the same mode.

## The CMOS byte (coreboot-patches/0001)

A small coreboot driver, `drivers/payload_select`, picks the payload from
CMOS offset `0x6c` (`CONFIG_PAYLOAD_CMOS_SELECT_OFFSET`):

| Value  | Written by | Meaning |
|--------|------------|---------|
| `0xC5` | menu, "UEFI (edk2)" | boot edk2 **once** (coreboot clears it) |
| `0xB7` | coreboot, before LinuxBoot | "LinuxBoot is starting"; the menu clears it once it's on screen |
| other  | | boot `img/linuxboot` (edk2 if it's missing) |

So if LinuxBoot ever hangs or crashes before the menu, **the next power-on
boots UEFI by itself**, and the one after tries LinuxBoot again. A bad
LinuxBoot can't lock you out of the machine.

## Using the menu

Keyboard: Up/Down, Enter, or 1-9. Gamepad: d-pad or left stick, A or Start
(Xbox 360/One pads through `xpad`, other USB pads through hid-generic). Any
input stops the countdown. USB sticks plugged in while the menu is up are
added. With no Crimson partition found, it counts down 10 s to UEFI.

Crimson boots the way its UEFI "Sion" entry does: if `sion/video-boot.cpio`
(the push-480i-boot kit) is on the partition, it's passed as the initrd with
`video=HDMI-A-1:2880x480M@30i`. Extra kernel options can go in
`sion/linuxboot-cmdline.txt` (one per line, `#` comments). The kernel also
gets `sion.loader=linuxboot`.

Because Crimson is kexeced, it starts **without UEFI**: no EFI variables.
`sion update` needs `--direct` there (its trial mode uses BootNext), and
push-480i-boot's installer (efibootmgr) only works when booted through UEFI.

## Building

```sh
./build.sh            # work/coreboot_bootmenu-fizz-mrchromebox_<date>.rom
VIDEO=native ./build.sh            # no 480i EDID: the display's own mode
CONNECTOR=HDMI-A-2 TIMEOUT=5 ./build.sh
```

Needs git, Go 1.22+, Docker and the usual kernel build tools. coreboot and
edk2 build in the `coreboot-sdk` image, which clones edk2 and iPXE during
the build. Behind a proxy, `HTTPS_PROXY` is passed into the container, and
`SDK_CA=/path/to/ca.crt` adds a re-signing proxy's CA.

`menu-2880x480.png` is the framebuffer as the GPU sends it and
`menu-tv-preview.png` is the same squeezed to 4:3, roughly what the TV shows.

## Flashing

As for any MrChromebox full ROM: write protect off (WP screw on fizz),
**back up first**, carry the VPD over, flash the BIOS region only:

```sh
sudo flashrom -p internal -r backup.rom
cbfstool backup.rom read -r RO_VPD -f vpd.bin
cbfstool coreboot_bootmenu-fizz-*.rom write -r RO_VPD -f vpd.bin
sudo flashrom -p internal --ifd -i bios -w coreboot_bootmenu-fizz-*.rom
```

Keep a recovery path (SuzyQ/CCD or a SPI clip plus the backup).

## Tested (cloud, not on the Sion yet)

- The fizz ROM builds (MrChromebox-2609.0, edk2 26.09.2): edk2 2.2 MB,
  LinuxBoot 5.9 MB, 4.3 MB of the COREBOOT region still free.
- Under real coreboot in QEMU (`emulation/qemu-q35`, SeaBIOS standing in for
  edk2): coreboot boots LinuxBoot; the menu's UEFI entry reboots into the
  other payload once, then LinuxBoot again; resetting while LinuxBoot is
  still starting makes the next boot fall back (seen in two of three tries;
  the odd one booted LinuxBoot again).
- The same LinuxBoot kernel in QEMU: menu, keys, the widened font at a
  2880x480 framebuffer (32x32 font, 90x15 text, normal shapes once the TV
  squeezes it to 4:3), and the Crimson entry kexecs the real Sion
  `BOOTX64.EFI` through to its login prompt.
- Not testable here: i915 and the 480i EDID on real hardware (QEMU's VGA
  refuses interlaced modes), the CMOS byte surviving on the Sion, edk2
  booting after it.
