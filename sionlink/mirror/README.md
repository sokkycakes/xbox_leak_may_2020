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
  on USB; it also filters the 3D commands and assembles each bundle, in frame order, so the game's
  thread only copies them out (`MIRROR_SYNCGX=1` does it all on the game's thread, as before). While two bundles are still waiting, frames are left out the way `--half` leaves them
  out, so the DS stays near live after a big VRAM upload instead of falling behind. The DS's
  reports also say which frame is on its screen; while more than four bundles sent are not shown
  yet, frames are left out too (with display capture, where none can be, the game waits). Without
  that, the DS's 64 KB buffer stayed full after a slow stretch and the picture, and so the pad,
  ran up to a second late.
- Cloud test: `--serve sim:PATH` listens on a unix socket instead. A second run,
  `mirrorrun mirrorplay.nds N --dspico PATH`, runs the client with a simulated DSpico
  (`harness/dspico_sim.cpp`, patched into melonDS's card slot through `CardHook`). The sim answers
  the DSpico card commands the way its firmware does, enumerates the client as a USB host, and
  caps the data at `--usbrate` bytes a frame (default 12000, about 700 KB/s, a guess until we
  measure a real DSpico). `--realtime 1` paces both runs at the DS's frame rate.
  `DSPICO_TRACE=1` prints the sim's traffic per frame, `DSPICO_PROF=1` where the DS's CPU is
  after frames in which it never looked at USB. `MIRROR_NOHOLD=1` turns off the hold above,
  `MIRROR_BEHINDLOG=1` prints how far behind the DS is twice a second.
- Skipped frames' 3D commands at 30 fps and in catch-up bundles: the runner keeps a shadow of
  the geometry engine (`harness/geostate.h`, melonDS's own rules) and, when it knows the whole
  state, sends a short rebuild of it (matrices, stacks, lights, material) instead of the kept
  commands. `MIRROR_GEOCHECK=1` compares the shadow with melonDS every frame.
- The DS reads the DSpico's events when its IRQ line says there are some, and otherwise every
  4 ms (every 1 ms while those reads find events): each card command is a chance of the hang
  where the DSpico stops answering until the DS is switched off.

## On the TV too (`kit/sion/tv.sh`)
`--tv drm` puts both screens on the Sion's TV, stacked, as big as fits the title-safe area of
the mode the TV is already in (720x480: 324x216 each), with the same KMS calls as xbox-av. It
takes the display from xbox-av the way a title does (`T`) and hands it back with its last
picture (`S`), so the TV never resyncs. The game then runs at the TV's pace: each frame waits
for the one before to be on screen. `--sound alsa` plays the game at 48 kHz on the dashboard's
sound device (`$AUDIODEV`, the TV's HDMI) through the kernel's PCM calls, and steers melonDS's
sample rate by how full its buffer is (60.00 Hz TV against the DS's 59.83). A frame that takes
longer than the TV's (a course loading) would leave the game behind the TV for good and empty
that buffer (run 11: about once a minute), so the frame after one isn't shown and the game
catches up. Not in a scene too heavy for 60 frames a second (run 12 left nearly every frame out
there and the TV stood still): there the game slows down, and its sound is made longer to
match (up to 18%, from how far apart the frames come) instead of breaking up. Not by playing
it slower, which lowered the pitch (run 15): `stretch.h` overlaps 16 ms pieces of it a little
more, each placed where it lines up with the sound before it, so the pitch stays (`--stretch
0`: short gaps instead). The speed line gives the average and
longest frame, how many weren't shown, and how busy each thread is (`CPU:`). `tv.sh` stops the
dashboard, runs the mirror with both, and starts the dashboard again when it stops
(`sh tv.sh stop`). Cloud tests: `--tv dump:FILE` (every 60th picture), `--sound wav:FILE`,
`MIRROR_TVPACE=1` paces the dump like a 60 Hz TV, `MIRROR_FRAMEHZ=60` paces without a TV,
`MIRROR_STALLTEST=N:MS` makes every Nth frame MS ms slower, `MIRROR_SOUNDLOG=1`,
`MIRROR_SOUNDFILL=FRAMES` (default 2560, 53 ms).

## As a runtime (`--hotplug`, `--pads`, `--save`)
Steps toward DS games as game cards on the Sion (plan: `ds-runtime-plan.md` in the project
files). `--hotplug 1` starts the game at once: a thread waits for the DS, and a DS can join,
leave and join again at any time. Until one is there no bundles are made; the 3D commands
still go through the geometry shadow as if sent, and the 3D registers that hold state (fog,
toon and edge tables, clear color, DISP3DCNT) are kept, so the full bundle a DS starts with
sets all of it. Cloud test: a DS joining mid-race shows the game's pictures from its
second frame on. A DS that's there but takes nothing (switched off with its DSpico still
plugged in, or its client stuck) is handled the same way once it has taken nothing for 2 s:
the frames go nowhere, as with no DS, until it takes data again and gets a full bundle.
Before, every frame queued its 3D writes behind the bundle it was stuck on, and run 18 grew
to 6.8 GB in 27 minutes until the kernel killed it; stopping in that state waited for the DS
for good. Cloud test (`DSPICO_STALL=N:M`, the simulated DSpico taking nothing for M frames
from frame N): a 30 s stall, memory flat (it grew 270 MB before), the DS back 4 frames
behind with one 284 KB full bundle; quitting and a reset from the menu during one work.
`--pads 1` lets the Sion's own pads play (`pads.h`: SDL2 game controllers,
its HIDAPI drivers over libusb as in xbcompat; build SDL with `harness/build-sdl.sh`):
buttons by position as on a DS, the right stick moves a stylus drawn on the TV, the right
trigger touches, and holding Back for 1.5 s opens the game's menu (below). Buttons from the DS,
the pads and `--input` all count. `MIRROR_PADTEST=FILE` plays a virtual pad in cloud tests
(a key script played as pad buttons gives the same recording). `--save FILE` loads the
game's save memory and writes it back 2 s after the game last wrote to it, through a temp
file and a rename.

## The game's menu (`menu.h`)
Holding Select for 1.5 s (Back on the Sion's pads, or Select on the DS over the link) stops the
game and opens its menu on the TV, as Game Boy Player and Virtual Console do; the Guide button
stays the Guide's. Until the 1.5 s are up the game gets Select as usual. The menu is drawn over
the dimmed picture in Frolick's look (its slate panel and Fira Sans, the focused row light with
dark text, the Blades' A and B buttons; `menuart.h` is baked from Frolick's art by
`mkmenuart.py`), with the game's banner icon and title and a line on how the DS link is doing:
- Resume (also B, Back, Start or Select)
- Screen layout: top screen only (the first time), bottom screen only, stacked, side by side,
  top screen big or bottom screen big (the other at half its size beside it), seen behind the
  menu as it changes; kept for the game in `GAME.layout`
- Swap screens (not with one screen alone)
- Quick save: one per game, `GAME.state` beside the save (melonDS's savestate, zlib, and the
  2D and 3D registers' last writes, which the DS can't read back), packed and written on a
  thread of its own while the game goes on; "Quick save made" shows at the foot of the TV.
  It waits for a frame where the game has no 3D commands half sent (MKDS: 2 frames).
- Quick load (asks first): melonDS takes the quick save, and so does the game's save memory,
  as melonDS's own savestates do; the save as it was is kept as `GAME.sav.before-load`.
- Reset game, Back to dashboard (both ask first)

The pads work it the dashboard's way (A picks, B goes back, the d-pad or left stick moves);
the DS the DS's way (A, B, the d-pad). The sound stops while it's up, and the DS gets a bundle
with nothing in it every quarter second by the clock, so it keeps its picture instead of
showing its link page. The TV gets a new picture only when the menu changes: drawing it every
frame took a whole core on the Sion, and the DS's bundles, counted in pictures then, came more
than a second apart (run 18: the DS's link page after each long menu, and a full frame after).
Its time is left out of the speed line's seconds and CPU shares. The buttons that close it stay out of the game until they're let go. After a quick load
or a reset the DS gets a full bundle of the new state: first zeros for any command it was part
way through, then the new 3D registers and the whole geometry state as melonDS has it (moving
the matrix stack pointers from where the old state left them), then the frames as usual. Cloud
test (MKDS demo, DS joined over the simulated DSpico): every DS frame matches an emulator frame
after the quick load and after the reset, from the 5th frame on (as when a DS joins); before
that it shows the DS's VRAM being cleared and one frame with the 3D a frame off.

## As an Xbox title (`kit/sion/install-title.sh`)
A DS game installed this way shows up in the Xbox dashboard's Games and in Frolick like any
game, and runs in the DS runtime when launched:
`sh install-title.sh GAME.nds ["Name"] [id] [GAMECODE]` on the Sion, from the kit folder
(the defaults are the Mario Kart DS kiosk demo's; `--remove [id]` takes it off again). It is a
Kazeta cart with `Runtime=nds` on the Sion's own disk (`/data/sionlink/carts/ID.kzi` and
`ID/GAME.nds`) made into a title the way xbcompat's `wincart.c` makes a Windows cart on the
game card into one: `dstitle.py` writes `E:\Games\ID\default.xbe`, a stub with the cart's
name, a title ID from its Id (`SN` + CRC-16) and the ROM banner's icon as its title image
(the same bytes `wincart.c` writes, checked), and a `.wincart` beside it naming the cart. The
xbcompat started for the stub execs `wincart-run` with the cart, which hands DS carts to
`dscart-run` (one line the installer adds, keeping `wincart-run.pre-ds`); that runs
`mirrorrun-gl` from `/opt/sionlink` as `tv.sh` does, with the save, the quick save and the
layout in `UDATA/<title id>/000000000001/`, so the Memory screen lists them under the game's
name and icon. Back to dashboard in the game's menu ends it, and the dashboard comes back as
after any title. Log: `/data/sionlink/logs/ID.log`. Tested in the cloud on a mock of the Sion's
paths: install, launch through `wincart-run`, reinstall, remove. The installer restarts the
dashboard as `tv.sh` hands back to it, keeping `xbox-av`: with `S98xbox stop` and `start` the
first install left the Sion with no display keeper until the next boot.

## OpenGL (`--renderer gl`)
melonDS's OpenGL renderer draws the game's 3D (and puts the screens together) on the GPU,
instead of the 3D thread the software renderer needs a CPU core for. The mirror opens a GL
3.2 core context through EGL with no window (Mesa's surfaceless platform, `libEGL.so.1`
loaded at run time, so a build without it still runs and just draws in software) and reads
both screens back for the TV, a frame late (into a buffer the GPU fills when it gets there:
copying the frame just made would wait for the GPU to finish it). Display captures stay on
the GPU until something reads them; when one comes into VRAM (melonDS tells the mirror,
`MirrorCaptureSynced`), a capture the DS made too goes into what the DS is known to have,
so a copy of it is never sent over the DS's own, and nothing waits for the GPU to get it.
The DS gets the same bundles as with software (MKDS demo, 2300 frames, byte for byte); only the TV picture differs, in how the GPU rasterizes (a few thin
lines on the bottom map, texel edges). Build with `GL=1 harness/build.sh` from a melonDS
built with `-DENABLE_OGLRENDERER=ON` (see build.sh). The Sion kit has it as `mirrorrun-gl`
(64-bit, glibc linked, the rest static), and `tv.sh` uses it unless told `soft`: run 16
held 57-60 fps in the demo's heavy scenes with the DS connected and the sound never ran dry,
where software (run 13) dipped to 53 with gaps, on half the CPU. The speed line's `waits:`
says how long a frame spent not running (waiting for the GPU, the DS or a lock).

## Not handled yet
- Main-memory display mode (DISPCNT mode 3).
- Compression on the client (the sizes above with zlib are what a link would carry).
- A bundle with a lot of VRAM (a scene change) can overrun the client's VBlank, so one frame
  shows part of the old VRAM.
