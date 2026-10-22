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

## Building
- melonDS at `melonds/BASE` with `melonds/mirror-tap.patch` applied, built with
  `cmake -B build -DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF`. Then run `harness/build.sh`
  (set MELONDS=path). The tap and runner are GPL with melonDS.
- `ds/`: BlockSDS (`make`, BLOCKSDS set). The client and `common/` are our own code.

## Not handled yet
- Registers changed mid-frame (HBlank DMA effects): only the start-of-frame value is sent.
- Main-memory display mode (DISPCNT mode 3).
- The bundle is uncompressed, with 8 bytes per 3D write. Packing GXFIFO runs and compressing
  should cut it a lot.
