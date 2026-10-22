# Screen mirroring test (DS second screen, step 1)

The plan is in the project files at `sionlink/ds-second-screen-plan.md`. A DS game runs in
melonDS on the Sion. A real DS shows both screens by replaying what the emulated game did to its
display hardware, instead of receiving video. This directory is the cloud test of that idea. Two
melonDS instances are used, and there is no link yet:

1. `mirrorrun ROM N --record mirror.bin` runs the game in melonDS. It writes one **bundle** per
   frame (format: `common/mirror_format.h`) and saves every frame's pixels.
2. `ds/` is the replay client, a BlockSDS .nds. It holds `mirror.bin` in NitroFS and replays it on
   the DS's own 2D/3D hardware: 3D writes during the visible lines, then 2D state in VBlank.
3. A second `mirrorrun` runs the client and saves its frames. `compare.py` matches each emulator
   frame with the client frame that shows the same bundle and counts differing pixels.

`harness/test.sh ROM FRAMES WORKDIR [INPUT]` does all three. Input scripts are in
`harness/inputs/`.

## What a bundle carries
A bundle is sent at the emulated VBlank start, and the client applies its 2D part in its own
VBlank IRQ.
- **3D:** every ARM9 write to DISP3DCNT and 0x04000320-0x040006A3 (geometry FIFO, command
  ports, 3D state tables), in order, up to the emulated VBlank start. The real DS's
  geometry engine re-runs them.
- **2D**, as of the start of the frame:
  - display registers of both engines (only those that changed, plus scroll/affine/capture
    registers whenever they were rewritten)
  - POWCNT1
  - VRAMCNT
  - changed 32-byte palette and OAM rows
  - changed 512-byte VRAM blocks of every bank
- **Mid-frame changes:** 2D registers the game changes partway down the screen, replayed by the
  client from an HBlank IRQ.
- **Display captures** aren't sent as pixels. The client's own hardware redoes them.
- **POWCNT1** goes in order with the 3D writes, because it gates the 3D engines, and also
  with the 2D state, for the screen swap.

## Building
- melonDS at `melonds/BASE` with `melonds/mirror-tap.patch` applied, built with
  `cmake -B build -DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF`. Then run `harness/build.sh`
  (set MELONDS=path). The tap and runner are GPL with melonDS.
- `ds/`: BlockSDS (`make`, BLOCKSDS set). The client and `common/` are our own code.

## Results (2026-10-22)
All 12 BlockSDS demos tried match pixel for pixel after the first boot frame. That covers 2D
backgrounds and sprites, 3D, HBlank DMA effects, display capture with 3D on both screens, and
blending. 2D and light 3D cost well under 2 KB a frame. Dense 3D models cost 42-45 KB a frame
raw and 5-17 KB with zlib. Details are in the project files at `sionlink/mirror-test/`.

## Options
- `--half 1`: 30 fps. Every other frame is sent and the client shows each for two. A skipped
  frame's 3D commands are parsed and only the state they leave behind (matrices, material,
  lights, polygon/texture attributes, color, viewport) is sent; frames with display capture are
  all sent.
- `--gxdelta 1`: 3D commands that repeat from the previous bundle are sent as copies of it.
- `--gamecode XXXX`: for prototypes whose game code melonDS's cart list gives to another game.

Mario Kart DS (kiosk demo, menus and a race) with `--gxdelta 1`: every frame pixel-identical at
60 and 30 fps, the race at about 40 KB/s with zlib.

## Live link (Sion -> DSpico USB -> DS)
With no `mirror.bin` in NitroFS or on the SD card, the client turns blue and becomes a USB device
on the DSpico's USB port (`ds/source/usblink.c`, driven through the DSpico's USB card commands).
It is a vendor-class device, VID 1209 PID 0001 (pid.codes test PID): bundles come in on bulk OUT
0x01, and once a frame the DS sends its pad and touch state (`mirror_input`) on bulk IN 0x81.

- `mirrorrun ROM N --serve usb` is the Sion side. It streams each bundle to the DSpico through
  libusb and plays the game with the DS's pad. A thread does the sending, so the game never waits
  on USB. While two bundles are still waiting, frames are left out the way `--half` leaves them
  out, so the DS stays near live after a big VRAM upload instead of falling behind.
- Cloud test: `--serve sim:PATH` listens on a unix socket instead. A second run,
  `mirrorrun mirrorplay.nds N --dspico PATH`, runs the client with a simulated DSpico
  (`harness/dspico_sim.cpp`, patched into melonDS's card slot through `CardHook`). The sim answers
  the DSpico card commands the way its firmware does, enumerates the client as a USB host, and
  caps the data at `--usbrate` bytes a frame (default 12000, about 700 KB/s, a guess until we
  measure a real DSpico). `--realtime 1` paces both runs at the DS's frame rate.
  `DSPICO_TRACE=1` prints the sim's traffic per frame.

## Not handled yet
- Main-memory display mode (DISPCNT mode 3).
- Compression on the client (the sizes above with zlib are what a link would carry).
- Real hardware: the live link has only run against the simulated DSpico.
- A bundle with a lot of VRAM (a scene change) can overrun the client's VBlank, so one frame
  shows part of the old VRAM.
