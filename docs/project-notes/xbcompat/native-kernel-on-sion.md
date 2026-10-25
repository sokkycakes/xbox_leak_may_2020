# Could the Xbox kernel run natively on Sion?

Short answer: the kernel itself could be ported to boot on Sion (Kaby Lake Chromebox, UEFI via MrChromebox). It's an i386 NT-derived kernel and a modern Intel CPU still runs 32-bit protected-mode code fine. The problem is that **games don't only talk to the kernel**. Every XBE statically links D3D8, DirectSound and the input libraries, and those poke NV2A and MCPX registers directly. Boot a ported kernel on Sion and the first title would still write NV2A push buffers to an address with no GPU behind it.

Paths below are under `xbox trunk/xbox/private/ntos/` in the repo.

## Blockers, from easiest to hardest

| Area | What the kernel assumes | What Sion has | Difficulty |
|---|---|---|---|
| CPU mode | 32-bit i386, no PAE, flat model | x86_64 CPU, runs 32-bit protected mode fine | Easy |
| Boot path | `bootx/bldr32` (`startup.asm`, `romcpy.c`, `enckey.h`): encrypted flash ROM, MCPX secret boot ROM, decrypts and maps the kernel | 64-bit UEFI | Medium: write a UEFI app that exits boot services, drops to 32-bit protected mode, loads the kernel image and builds a fake loader block. Drop the ROM crypto and signature checks |
| Memory | Fixed 64/128 MB map (`mmx/mi.h`: `MM_HIGHEST_PHYSICAL_PAGE 0x3FFF`/`0x7FFF`), NV2A instance memory carved from the top, D3D owns physical page 0, memtop set via NV2A host-bridge reg `0x87` | 4–8 GB, UEFI memory map with holes, MMIO below 4 GB | Medium: feed a fake 64/128 MB region and fix the PFN DB setup |
| Interrupts and timers | `halx/i386`: 8259 PIC (`ix8259.inc`), APIC explicitly turned off (`xxhal.c`), ACPI PM timer at MCPX I/O base (`pcisetup.c`, `pmtimer.asm`) | PCH still emulates 8259 and has a PM timer, but at different I/O/PCI locations | Medium: re-point the HAL |
| SMBus / SMC / EEPROM | `halx/i386/smbussci.c`, `mcpxsmb.inc`: SMC (power, LED, tray, fan), EEPROM (keys, region, video settings), video encoder all on MCPX SMBus | Different SMBus controller, no SMC, no EEPROM | Medium: stub the SMC, fake the EEPROM in RAM |
| Storage | `idex/`: MCPX PATA, DVD drive with Xbox security handshake, FATX partitions at fixed offsets | M.2 SATA (AHCI) SSD, USB | Hard-ish: new AHCI driver, FATX image on the SSD, no DVD |
| USB / controllers | `dd/usb/ohcd`: OHCI only, XID protocol for pads | xHCI only | Hard: write an xHCI host driver and HID-to-XID mapping |
| Video output | `av/` + NV2A + Conexant/Focus/Xcalibur encoder over SMBus | Intel UHD 620, eDP/HDMI via display engine | Hard: a mode-setting driver for Intel display (i915-sized problem) |
| Audio | MCPX APU (VP/GP DSP) + AC'97 | Intel HDA over HDMI | Very hard (see below) |
| Network | nForce NIC (`xnet/enet`) | Realtek NIC | Medium, optional |
| **GPU** | Titles link D3D8 that builds NV2A push buffers and writes NV2A MMIO at `0xFD000000` | Intel Gen9.5 GPU | **The real blocker** |

## Why the GPU and audio are the wall

The kernel is only the OS. Graphics and sound drivers live **inside each game**. D3D8 and DSound are statically linked into every XBE and program the NV2A and MCPX APU directly, including shader microcode (NV2A vertex programs and register combiners) and APU DSP code. Nothing in the kernel can redirect that. There are only two ways around it:

1. **Trap and emulate the hardware**: unmap `0xFD000000` and catch every access. That is an NV2A emulator, which is what xemu is. It just runs on bare metal instead of in qemu.
2. **Patch the game's linked libraries**: find D3D8/DSound by signature and replace them with your own code. This is exactly what xbcompat already does (Proton-style).

Either way, the "native kernel" part doesn't remove the hard work, which is translating NV2A to a modern GPU. On bare metal you'd also have to write that GPU backend against Intel hardware with no Mesa, which is much worse than writing it against GL/Vulkan on Linux.

## A realistic path

- **Recommended:** keep xbcompat as the compat layer. Run it on Sion under the Buildroot image the project already boots (KMS + Mesa iris + ALSA HDMI). That gives you a "boots straight into Xbox" appliance feel without a kernel port. Linux supplies the xHCI, AHCI, Intel display, HDA and GPU drivers, and xbcompat supplies the kernel exports plus D3D8/DSound translation. Remaining work is the same as now: DirectSound, input, shaders and push buffers.
- **If you want the real kernel as a curiosity:** port it to boot on Sion with UEFI shim → 32-bit handoff, fake EEPROM/SMC, re-pointed HAL, a framebuffer from UEFI GOP for text output, and AHCI + xHCI drivers. That gets you to the dashboard's kernel init, and perhaps to running homebrew that doesn't use D3D. It's months of driver work. Retail games still won't render without solving the NV2A problem above.
- Booting it in a VM with emulated MCPX/NV2A is xemu, which was already ruled out.
