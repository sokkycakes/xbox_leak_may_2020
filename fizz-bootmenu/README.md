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
                     └─ otherwise ── LinuxBoot ── i915 lights the TV at 480i ─┐
                                    nothing held ──── kexec Crimson straight away
                                    button held ───── menu:
                                                   ├─ Crimson                  kexec EFI/BOOT/BOOTX64.EFI
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

**The menu is hidden by default.** With nothing held, LinuxBoot boots Crimson
as soon as it can (it looks for a held button until 2.5 s after the kernel
starts, so a USB pad has time to show up). To get the menu, hold one of these
while the Sion powers on, until the menu appears:

- the **power button**: press it once to power on, then press it again and
  hold it while the screen is still black, until the menu appears (tested
  on the Sion; the press that powers it on doesn't count, the EC has
  already handled it). Don't hold it past about 8 s, or the EC forces
  power off;
- any **key** on a USB keyboard;
- any **button** on a wired USB gamepad (a wireless pad usually isn't
  connected that early).

Shown that way, the menu waits for a choice (no countdown). If Crimson isn't
found or fails to start, the menu comes up by itself. `SHOW=always ./build.sh`
builds the old behaviour: the menu every boot with a countdown.

In the menu: keyboard Up/Down, Enter, or 1-9. Gamepad: d-pad or left stick, A or Start
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
SHOW=always ./build.sh             # menu every boot, with a countdown
HOLDWAIT=4000 ./build.sh           # look for a held button for longer (ms)
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
  2880x480 framebuffer (32x16 font, 90x30 text, normal shapes once the TV
  squeezes it to 4:3; `crimsonboot.font=tall` doubles the height), and the Crimson entry kexecs the real Sion
  `BOOTX64.EFI` through to its login prompt.
- Not testable here: i915 and the 480i EDID on real hardware (QEMU's VGA
  refuses interlaced modes), the CMOS byte surviving on the Sion, edk2
  booting after it.

## Trying it first without flashing

`test-from-uefi/` boots the same menu once from today's UEFI (BootNext), no
flashing. Tested in QEMU with OVMF: UEFI started `crimsonboot.efi`, the menu
counted down and kexeced the real Sion image, which also kept its EFI
system table.

## On the Sion (2026-11-26)

The no-flash test ran on the Sion: UEFI started the menu, the TV showed it at
480i, and it booted Crimson (2880x480i, with UEFI variables still there).
The first build's 32x32 font looked squished; the ROM now uses 32x16, and
re-widens the font when i915 replaces the firmware's framebuffer.

`flash-kit/` flashes it from Crimson: `sh sion-flash.sh` as root. It backs the
firmware up to /data/sion, refuses unless the FMAP matches and write protect
is off, carries RO_VPD, SMMSTORE and RW_MRC_CACHE over, writes the BIOS
region only and verifies. The flashrom in it is a static 1.4.0 build
(internal programmer only).

## Hidden menu (2026-11-26, ROM `…20261126d`)

Tested in QEMU: with nothing held the menu never draws and Crimson boots
directly; a key held during the window brings the menu up without a
countdown. Not testable here: the power button (the EC's switch byte at
LPC 0x930, bit 1, only exists on the real board) and a held Xbox pad.
Flash it with the same `flash-kit/` (`sh sion-flash.sh`).

Silent plain boot (ROM `…20261126d`): crimsonboot is now PID 1 itself
instead of u-root's init, which printed its banner, a log line and let the
kernel's cgroup warnings onto the TV. It does the same root filesystem
setup with the console log level at 1, and the payload uses `loglevel=1`,
so a plain boot shows nothing between power-on and Crimson's animation.

## Boot animation at power-on (ROM `…20261126f`)

With nothing held, LinuxBoot plays the native bootani as soon as i915 is up
(about 2.5 to 3 s after power-on), loads Crimson while it plays, and kexecs
it when the animation ends:

- **Where it lives:** the animation and its graphics driver sit on the SSD,
  in SIONBOOT's `sion/bootani/` (about 16 MB), not in the flash: bootani
  (KMS, sound through the kernel's PCM ioctls, no libasound), a trimmed Mesa
  (iris for gfx9 / Kaby Lake only, Kaby Lake perf metrics only, no LLVM, LTO;
  `mesa-patches/`, Mesa 24.0.9) and the glibc it was built with, run through
  its own loader. The ROM only gained the kernel's HDMI audio and the i915
  patch (about 0.1 MB); about 4.4 MB of the COREBOOT region stays free.
  Install with `bootani-esp/sion-install-bootani.sh` (from Crimson, as
  root; `--remove` takes it off). Without it LinuxBoot boots Crimson with a
  black screen as before.
- **No black gap:** bootani copies its last frame into the console
  framebuffer (`--to-fbdev /dev/fb0`, which i915 keeps in stolen memory),
  fbcon only takes the screen once something prints
  (`FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER`), and
  `linux-patches/0001` with `i915.kexec_keep_display=1` leaves the pipe
  running through the kexec. Crimson's i915 (fastboot, same EDID) takes the
  plane over as its initial framebuffer, and Crimson gets
  `sion.bootani=done`, so it skips its own animation and keeps that frame up
  until the dashboard draws (needs the Crimson image from the "Speed up Sion
  boot time" work; an older image plays its animation again). It also gets
  `sion.esp=<PARTUUID>` of the SIONBOOT it came from.
- **Menu:** a button held at power-on, or pressed any time during the
  animation, stops it and opens the menu. If bootani fails, Crimson is
  booted without `sion.bootani=done` and plays its own.
- **Sound:** HDMI through Intel HD Audio (built into the LinuxBoot kernel),
  first HDMI port, retried while the codec probes.
- Options: `crimsonboot.bootani=0` (no animation),
  `crimsonboot.bootaniargs=--short` (comma-separated bootani options).

Tested in QEMU with a software Mesa (softpipe) standing in for iris, from
the ESP: the animation starts and renders (slowly), a key opens the menu
over it, and with a quick stand-in for bootani the frame it leaves in fb0
stays on screen and LinuxBoot kexecs Crimson with `sion.bootani=done` and
`sion.esp=` when it exits. iris, HDMI audio and the kept display across
kexec only run on the Sion itself.

## Graphical menu at 240p (ROM `…20261126g`)

The menu (held button at power-on, or a press during the animation) is now
drawn with a font and pictures instead of console text (`crimsonboot/gfx.go`):

- **240p super resolution:** while the menu is up the screen switches to
  2880x240p, the 2880x480i mode made progressive: same 54 MHz clock and
  15.734 kHz line, 262 lines, about 60.05 Hz. The menu is drawn on a
  320x240 canvas (square pixels on a 4:3 TV) and each pixel is shown 9 wide
  and one line tall. If i915 refuses 240p it draws the same picture at
  480i. `crimsonboot.menures=480` skips 240p, `crimsonboot.menu=text`
  brings back the text menu (also used by itself when DRM fails).
- **Handover:** before an entry runs, the last menu picture ("Starting
  Crimson") is copied into the console framebuffer and the screen goes back
  to 480i, so Crimson starts from that picture and not from text.
- **Theme** (all optional) in SIONBOOT's `sion/menu/`: `font.ttf`,
  `font-bold.ttf`, `background.png` (scaled to 320x240), `logo.png`
  (replaces the CRIMSON title, fits in 288x40), `icons/crimson.png`,
  `uefi.png`, `shell.png`, `reboot.png`, `poweroff.png` (16x16) and
  `theme.conf` with `accent=`, `text=`, `dim=`, `status=`, `top=`,
  `bottom=` (`#rrggbb`; top and bottom are the background gradient) and
  `size=` (entry text size, default 13). Without it: Go fonts, a dark red
  gradient and the Crimson red. `menu-240p-preview.png` shows the default.
- Costs about 0.2 MB of flash (Go fonts and image code); about 4.2 MB of
  the COREBOOT region stays free.

Tested in QEMU (bochs): the menu draws through DRM, the shell entry hands
the screen back and the menu comes back after `exit`, and the "Starting
Crimson" picture stays up through the kexec. QEMU's display has no
interlaced modes, so the 480i to 240p switch itself only runs on the Sion.
