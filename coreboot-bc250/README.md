# coreboot for the ASRock BC-250 (proof of life)

A first coreboot port for the BC-250 (AMD Cyan Skillfish, the cut-down PS5
APU). It is built from the stock ASRock BIOS: every AMD blob in the image is
copied out of `BC250_3.00.ROM` unchanged, and coreboot replaces only the x86
side.

**Status: built and checked offline, never run on hardware.** This image
does not boot an OS. It answers one question: does the PSP accept a coreboot
image, train the GDDR6 with the stock ABL, and hand the x86 to our code?

## How it works

On this SoC the PSP (AMD's security processor) does memory bring-up before
any x86 code runs. It reads the PSP directory from flash, runs the ABL with
the APCB to train GDDR6, writes the results (the APOB) to DRAM, copies the
x86 "reset image" from flash into DRAM, and releases the CPU at the end of
that image. coreboot's Picasso support already boots this way, so the port
reuses that flow:

| Piece | Source |
|---|---|
| PSP directory: bootloader, TOS, SMU, ABL0-4, UMC, keys, ... (19 entries) | stock BIOS, byte-identical |
| APCB (memory and board config) | stock BIOS, byte-identical |
| APOB destination `0x4000000` | same as stock |
| Reset image | coreboot bootblock (stock: AMI PEI) |
| VBIOS `113-AMDRBN-003`, AMD GOP 2.6.0.0.2 | stock BIOS, extracted for later graphics work |

The stock image has no BIOS signing key or signature entries, so Platform
Secure Boot cannot be enforcing and the PSP should load an unsigned reset
image. That matches modded AMI images booting fine on this board.

## What the image does

The bootblock prints the CPU name and CPUID over serial, checks for the
APOB at `0x4000000` (proof the stock ABL trained memory), then:

- `coreboot-bc250-halt.rom`: halts.
- `coreboot-bc250-resetloop.rom`: resets every 5 seconds. Use this if you
  have no serial console. A board that resets every 5 s, fans spinning back
  up each time, is running coreboot. A board that never resets means the
  PSP refused the image or something died before the bootblock.

Serial goes to the Super I/O's COM1: 0x3F8, 115200 8N1. The stock BIOS's
own ACPI tables declare that port (`UAR1`, Nuvoton NCT6686D at config port
0x2E, logical device 2), so it is the most likely console. **Whether its TX
pin reaches a header or test pad on the board is not confirmed.** The FCH's
own UARTs are still selectable in Kconfig, but the stock BIOS configures
none of them.

## Before flashing anything

1. Read and keep your own chip's contents (BIOS_A1, 16 MiB):
   `flashrom -p internal -r stock-backup.bin`, and keep the hash.
2. Have a CH347 (or a CH341A confirmed at 3.3 V) and a SOIC-8 clip. Any of
   these images can leave the board dead until you clip-flash the backup
   back. Do not touch the 512 KiB SIO1_R chip.
3. Flash with an external programmer; `flashrom -p internal -w` from the
   stock BIOS may also work, but you lose the internal path if it fails.

## Rebuilding

```sh
./build.sh halt         # or resetloop / continue
```

The script fetches the pinned upstream coreboot commit, applies
`patches/`, downloads the public P3.00 dump (hash-checked) or uses one you
pass as the second argument, extracts the blobs, builds, and runs
`compare_psp_layout.py` to prove every stock PSP blob made it in unchanged.
Building the cross-compiler the first time takes about 20 minutes.

## LinuxBoot from USB

`linuxboot/` has a LinuxBoot kernel (Linux + u-root) that the stock UEFI
boots from a USB stick and that kexecs into Bazzite. It tests LinuxBoot on
this board without flashing. See `linuxboot/README.md`.

## Files

- `patches/0001` amdfwtool: PSP type `0x4F` (UMC firmware) and a
  `CyanSkillfish` platform that matches the stock layout (Picasso's layout,
  minus the soft fuse and PSB signature entries).
- `patches/0002` `soc/amd/cyan_skillfish`: the SoC, started from Picasso
  with all FSP code removed. Family 17h model 47h, SMU v10.
- `patches/0003` `mainboard/asrock/bc250`: the board and proof-of-life
  bootblock.
- `extract_stock_blobs.py`: pulls the PSP entries, APCB, EFS blobs, VBIOS
  and GOP driver out of a stock image.
- `compare_psp_layout.py`: checks a built image against the stock one.

## What the stock image says about the board

Found by decompiling the stock DSDT and AMD's CPM tables (no hardware):

- **PCI layout matches Picasso:** GPP bridges at 00:01.1-01.7, internal
  buses at 00:08.1 (GPU, xHCI, audio) and 00:08.2 (SATA), FCH at 00:14.x.
- **Serial:** COM1 on the Super I/O, see above. No FCH UART is declared.
- **AMD CPM GPIO tables are empty.** The stock firmware sets almost no
  GPIOs itself (one pin, GPIO 61), so the board relies on strap and reset
  defaults. That is good news for the port: there is little GPIO setup to
  reproduce. The OEM tables are AMD's "Piccolo" reference template.

## Known differences from the stock layout

- amdfwtool adds an APCB backup entry (`0x68`) pointing at the same APCB.
- The EFS IMC/xHCI blobs are stored at different (unaligned) offsets.
- The APOB NV region is 64 KiB at flash offset 0 instead of 8 KiB at
  `0xAB3000`. It is erased in both.

## What comes after proof of life

Everything AGESA does after memory training is missing: SMU setup and
waking the other cores, PCIe lane setup (DXIO) for the M.2 slot and the
RTL8111H, USB, SATA, GPU memory (UMA), interrupt routing and ACPI. The
stock image has these as `AmdNbio*ARI`, `AmdFabricAri`, `AmdCcxVhAri`,
`Fch*` and `AmdCpm*` modules, which are the reference to reverse-engineer
from. The SMU is a PS5-custom part and wrong mailbox writes are known to
hang the board.
