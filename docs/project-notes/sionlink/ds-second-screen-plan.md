# DS games on the Sion with a real DS/3DS as pad + second screen (plan, 2026-10-19)

Nothing here is built. This answers "what would it take" from the "DS second screen" thread.

**Recommended: mirroring as the screen path** (sokky asked about lockstep instead; see section 5
for why it doesn't work against a real DS). Pixel streaming stays only as the fallback for bottom screens mirroring can't cover (3D, display capture) and for a native
3DS client, whose GPU isn't a DS 2D engine.

## The short answer
Run a DS emulator on the Sion that shows the **top screen on the TV**. A small client on a linked
**DS or 3DS** sends buttons, stylus and mic over Sion Link and shows the **bottom screen**. The
emulator part already exists (melonDS runs fine on x86). The DS link already exists (sionlinkd,
real DS joins after the rate fix). What's new is three pieces: a bridge in the emulator, the
handheld client, and a screen protocol that fits through the DS radio. The hard part is
bandwidth, and the DS has a trick that solves most of it (see "Mirroring" below).

## Target experience (sokky, 2026-10-22)
Like IS-NITRO or a DS with a capture card on the TV: **both screens in your hand and both on the
TV**, the DS acting as a controller with screens. With only USB and Wi-Fi available, that means
the game runs on the Sion and the DS mirrors **both** screens:
- **USB (DSpico, ~17 KB/frame):** both engines mirrored, including **3D** by forwarding the
  frame's geometry command list (the real DS has the same 3D engine; textures go through the
  same VRAM mirroring and cache). Typically 10-40 KB/frame, so 60 fps for lighter scenes and 30
  for heavy ones. Main mode.
- **Wi-Fi (~3.5 KB/frame):** lighter mode, bottom screen (2D) mirrored only, top screen on the TV.
- The TV can be delayed a frame or two to line up with the handheld.
- Rejected: game running on the real DS streaming out. Display capture grabs only engine A and
  needs free VRAM, many display registers are write-only, the stream costs the game CPU and
  card-bus time, and every game needs a patch. Only an LCD hardware tap does that cleanly.
- No other wired port: the power port is power (plus audio on SP). The GBA slot (DS/DS Lite only)
  is a real bus that homebrew USB boards used (DSerial, from memory), but the DSpico covers it.

## Architecture
```
 Sion                                              DS / 3DS
 ┌───────────────────────────┐   Sion Link       ┌──────────────────────┐
 │ melonDS core (own process)│                   │ sion-pad client      │
 │  top screen  ─► TV        │  ◄── input ────── │  buttons, touch, mic │
 │  bottom screen ─► bridge ─┼── screen ──────►  │  draws bottom screen │
 │  audio ─► Sion speakers   │   (sionlinkd)     │  (lid closed = pause)│
 └───────────────────────────┘                   └──────────────────────┘
```
- **Emulator:** melonDS, run as its own process with a small headless frontend of ours. It
  takes input from the bridge instead of a keyboard and hands the bridge the bottom screen.
- **Bridge (in sionlinkd or beside it):** owns the link to the handheld, does frame pacing,
  encodes the screen, feeds input back to the emulator.
- **Client:** a homebrew .nds (DS, and 3DS in DS mode) or a native .3dsx (3DS with Luma).

## What we can reuse
- **sionlinkd + patched hostapd + iwl_ratefix.ko** (kit 12/13): the DS-only AP on the Sion, a
  real DS joining it, and the melonDS UDP hookup (`sionlinkd --melon PORT`) for testing without
  hardware.
- **The Sion Link launcher (sionlink.nds)** already pulls programs over our own protocol
  (`sion-dslink send FILE.nds`, staged in a 2.5 MB buffer, booted through common/slboot.h).
  Link Test and the FFCC client arrive this way, so the second-screen client is just one more.
- **Link Test (kit 13, commit 4bf0c7b0):** already a two-way Sion<->DS program with a Sion screen
  and a DS screen; 10 minutes in melonDS delivered 35,219 of 35,219 inputs at ~60/s both ways.
  The client is basically Link Test with a framebuffer added.
- **Game socket:** Sion-side games already talk to the link through /run/sionlink/game.sock
  (common/slgame.h); the bridge is one more game on it. Shared DS link code is dslib/sllink.{c,h}
  on branch claude/project-thread-dr8bfu (FFCC client 0ec32398, host 3bd4762b, which send pad
  state and screen words but no framebuffer yet).
- **3DS side:** the Sion Cards design (3ds-cards/design.md) and the planned UDP backend
  (ctr/slnet3ds.c) cover how a 3DS app finds and talks to the Sion.
- **xbcompat pad layer** ideas: the handheld shows up as a pad, same as the Console Link
  "handheld as a controller" option (console-link/console-link-brainstorm.md).

## Wire format (draft, 2026-10-22)
One **frame bundle** goes down per emulated frame, and one **input report** comes up per DS vblank.
Same format on USB and Wi-Fi; only the chunking differs.

### Down: Sion -> DS, one bundle per frame
Header: magic, frame number N, base frame (the last frame the DS confirmed, which the diff is
against), flags, total length. Then a list of records, each `[type][length][payload]`:

| Record | Payload | Typical size |
|---|---|---|
| `REGS` | engine A/B, list of (register offset, value) for display registers that changed: DISPCNT, BGxCNT, scroll, affine, windows, blending, master brightness, screen-swap bit | 20-100 B |
| `LINEREGS` | register + 192 per-scanline values, only when the game changes it mid-frame (HBlank DMA wobble/parallax) | 384 B each, rare |
| `VRAMMAP` | the 9 VRAMCNT bank-mapping bytes, when they change | 9 B, rare |
| `VRAM` | bank, 512-byte block index, compressed block | 0-2 KB normally, bursts on scene changes |
| `CACHED` | bank, block index, 64-bit hash: "load this block from your SD cache" | 12 B per block |
| `OAM` | engine, dirty 64-byte chunks (8 sprites each) | 0-300 B |
| `PAL` | engine, dirty 32-byte rows (16 colours), normal + extended palettes | 0-200 B |
| `GX` | the frame's 3D command list (packed geometry FIFO words), compressed, plus 3D state (DISP3DCNT, clear colour, fog/toon/edge tables when changed) | 0 (2D game) to 8-30 KB |
| `END` | checksum; commits the frame | 8 B |

Typical totals: 2D game 0.3-3 KB a frame (fits Wi-Fi), 3D game 10-35 KB (needs USB).
Audio stays on the Sion/TV.

### Applying it on the DS
The client buffers the whole bundle in main RAM and only applies it once `END` arrives, during
the next vblank (registers, mapping, OAM, palettes first; VRAM blocks and the `GX` list right
after), so you never see half a frame. The DS screen runs one frame behind the emulator; the
TV is delayed to match.

### Up: DS -> Sion, one report per DS vblank (~16 B, plus mic)
- Last fully applied frame number (doubles as the ack; anything newer that's missing is resent).
- Buttons (KEYINPUT + X/Y/lid), pen down, touch X/Y (raw 12-bit, so the game's own calibration
  works).
- Optional: ~130 B of 8 kHz mic samples, client stats (link quality, battery).
The report's arrival is also the **clock**: the Sion runs one emulated frame per report, so the
emulator stays locked to the DS's 59.83 Hz.

### Chunking
- **USB (DSpico):** the bundle is sent as 512-byte bulk transfers, matching the DSpico's
  512-byte endpoint buffers (card commands E9/EA). USB itself handles loss.
- **Wi-Fi:** the bundle is split into ~1 KB broadcast packets tagged (frame, chunk, count); the
  input report doubles as a NACK list for missing chunks.

## The hard parts

### 1. Bandwidth (the big one)
- A bottom screen is 256x192 at 15 bit = **96 KB per frame**, about **5.9 MB/s at 60 fps**.
- The DS radio does **1 or 2 Mbit/s**, and the Intel card only reaches a DS with broadcasts
  (no ACKs, no retries, see common/slproto.h). Measured in melonDS: **~220 KB/s** (3.3 MB in
  14.8 s) and a 17 ms ping; not yet measured on a real DS. That's about **3.5 KB per frame** at
  60 fps, so raw video is ~25x too big.
- **Plan A, pixels (works for every game, simplest):** 8x8 dirty tiles, palettized to 8 bit when
  the frame has ≤256 colours, LZ77 so the DS BIOS can decompress it for free. Static menus and
  map screens (most bottom screens) cost almost nothing. Scrolling or animated bottom screens
  will drop to 10-20 fps.
- **Plan B, mirroring (chosen):** the real DS has the same 2D hardware as the one
  being emulated. Instead of pixels, send the **state of the emulated 2D engine** that drives
  the bottom screen: its registers, OAM (1 KB), palettes (1 KB + extended), and the dirty 512-byte
  blocks of the VRAM banks mapped to it (C/D/H/I). The client copies that into its own engine
  and the DS draws the screen natively, **pixel-perfect at 60 fps**. A scroll is a 4-byte
  register write instead of a whole new frame. Uploading a new tileset costs a burst once, then
  nothing.
  - Gaps: when the game puts **3D or display capture** on the touch screen, mirroring doesn't
    cover it; fall back to Plan A automatically. Mid-frame raster effects (HBlank DMA into scroll
    registers) need per-line register capture. Both are detectable from the emulator.
  - The client mirrors **whichever engine the game routes to the bottom screen** (POWCNT1
    swap bit), mapping the same VRAM banks the game mapped (A/B/E/F/G for engine A, C/D/H/I for
    engine B). The client's own code lives in main RAM, so all VRAM is free for the mirror; the
    other engine shows the client's status on the real DS's top screen.
  - Hooks needed in melonDS: register writes for the bottom-screen engine (latched at vblank,
    plus per-line values when they change mid-frame), VRAM bank mapping changes, and dirty
    tracking on VRAM/OAM/palette writes. All inside the GPL emulator process; it hands the
    bridge a per-frame diff.
  - Later: 3D could be mirrored too by forwarding the geometry command list, but that's
    tens of KB a frame, so only for light scenes.
- **On a 3DS native client the problem mostly goes away:** it joins a normal 802.11g network
  (the Sion's home Wi-Fi, not the 1-2 Mbit DS-only AP), gets several MB/s, and its ARM11 can
  decode a real codec. Plan A at 60 fps just works there, since a native 3DS app can't mirror
  a DS 2D engine. A 3DS running the .nds client in DS mode mirrors like a DS.

### 1b. Wired option: USB through a DSpico (checked 2026-10-22)
The DSpico's stock firmware (github.com/LNH-team/dspico-firmware, docs/commands.md) already hands
the RP2040's USB device controller to DS homebrew through card commands: E8 (USB sub-commands
that map to TinyUSB's `dcd_*`: init, connect, open endpoint, start transfer), E9/EA (write/read
a 512-byte endpoint buffer, two per endpoint), EB (read an event; the cart IRQ line signals
one is waiting). So with a cable from the Sion's USB to the DSpico's micro USB:
- **No firmware change.** The client runs the USB device side itself (TinyUSB device stack on the
  DS, with the DSpico commands as its controller driver) and shows up on the Sion as a vendor
  bulk device; the Sion side is plain libusb in sionlinkd.
- **Speed:** RP2040 USB is full speed (12 Mbit/s), so roughly 1 MB/s of bulk data, about 5x the
  wireless link and ~17 KB per frame. Estimate, not measured. Every packet is acknowledged and
  retried by USB, so no loss handling, and latency is about 1 ms.
- **What it buys:** mirroring never stalls on tileset bursts, and the pixel fallback reaches
  playable frame rates. Raw 60 fps pixels (5.9 MB/s) still don't fit.
- **Costs:** a cable, DSpico-only (other flashcarts lack this), and the ARM9 spends some time on
  card-bus transfers. Works on a 3DS too when the DSpico runs in DS mode, since it's the same
  card bus (not tested).
- **How it fits:** a second transport under the same Sion Link protocol. sionlinkd picks USB when
  a DSpico client is plugged in and wireless otherwise; the client code above the transport
  doesn't change.

### 2. Packet loss
Broadcast-only means lost packets are never resent by the radio. Every update carries a frame
number; the client reports the last frame it has fully, and the bridge resends what's missing.
A slow background refresh rewrites every tile/block so damage always heals.

### 3. Latency and touch round-trip
- Buttons: DS sends its input once per vblank, so ~1 frame up, then the emulator frame. Fine for
  most games, about the same as a wired pad on a TV with game mode.
- Touch is worse because you see the result on the screen you touched: input up (1 frame) +
  emulate (1) + encode/send/draw (1-3) = **~3-5 frames, 50-80 ms**. Noticeable when drawing.
  Fixes: the client draws a local "ink" trail and cursor the moment you touch (like a phone
  keyboard echo), and mirroring keeps the downstream side short.
- **Frame pacing:** the DS screen runs at 59.83 Hz. Drive the emulator from the client's vblank
  heartbeat instead of the Sion's clock, so the two never drift and there's no judder or
  creeping lag.

### 4. What runs on the DS and how it gets there
- **Client (.nds):** libnds + our Sion Link wireless code, about the size of Link Test. Engine
  B (or A) drives the bottom screen as the mirror/bitmap target, the top screen shows status
  (link quality, game name, battery). Sends keys, touch X/Y, and optionally mic samples (for
  games you blow into).
- **Delivery:** no signing problem. The Sion Link launcher (sionlink.nds) runs from sokky's
  flashcart on a DS, or through TWiLight in DS mode (Bootstrap Off) on a 3DS, and pulls the
  client with `sion-dslink send`, the same way Link Test and the FFCC client arrive. Nintendo's
  Download Play RSA check never comes into it.
- **3DS:** best as a native .3dsx (or a Sion Cards card): bigger bottom screen (320x240, the
  DS image fits 1:1 with a border so touch maps exactly), far more CPU, normal Wi-Fi. A 3DS in
  DS mode runs the .nds client too, with DS radio limits.

### 5. Why not send the game and run it on both in sync
Asked in the thread (2026-10-19). Run the game on the Sion (melonDS) and on the real DS at the
same time, feed both the same input, and let each show its own screen. It doesn't hold up:
- **Lockstep needs two identical machines.** melonDS isn't cycle-exact with real hardware
  (cache, memory and 3D timing, when the ARM7 samples touch, RTC, timing-seeded RNG). The two
  drift apart within seconds to minutes, and a real DS can't be rolled back or re-synced, so
  one desync is permanent. Netplay lockstep works because both ends run the same emulator.
- **Running it on the real DS:** possible, from the flashcart through nds-bootstrap (TWiLight
  on a 3DS), with a small hook patched into the game to send each frame's input to the Sion.
  That's nds-bootstrap-level patching per game, not just another program for the launcher.
- **Catching a desync:** the hook would hash part of RAM each frame and send it with the
  input; the Sion compares with melonDS. That catches it, but only after the screens differ.
- **Recovering:** no good way. The emulator can't be made to match the DS, and loading a
  snapshot into a running real DS (both CPUs, 3D, sound, timers mid-frame) isn't practical.
  In practice, recovery means restarting the game on both.
- **Input timing:** both copies would have to read each input on the exact same cycle. The
  DS reads its own touch screen locally; the Sion gets it a frame or more later.
- **Size:** most DS games are 8-256 MB; the DS has 4 MB of RAM and the launcher's buffer is
  2.5 MB. The game would have to already be on the flashcart, and then the DS is just playing
  it alone.
- **What does work from the idea:** a cache. The client keeps VRAM blocks it has seen (by
  hash) on its SD card, so a tileset that comes back later is a few bytes ("use block X")
  instead of a burst. That cuts mirroring's worst case without any sync risk.

### 6. Licensing
Every mature DS emulator I know of (melonDS, DeSmuME, NooDS) is GPL. Keep it in **its own
process**: our headless frontend changes stay GPL and ship with source, while the bridge,
protocol and clients talk to it over a socket and can stay under our own licence. That matches
the xbcompat preference without a clean-room DS emulator, which would be a multi-year project.
Mirroring needs hooks inside melonDS's 2D engine code, which also stay in the GPL part.

## Rough order of work
1. **Emulator on the Sion:** headless melonDS with the top screen on the TV and both screens
   side by side as a fallback. Check full speed on the Sion's CPU (the x86 JIT should manage it).
2. **Pad only:** client sends input over Sion Link, emulator uses it, bottom screen still shown
   on the TV. Measures the real input latency and proves the link under load. Test in melonDS
   via `sionlinkd --melon` first, then a real DS (also gets the first real-hardware
   throughput number).
3. **Mirroring:** melonDS hooks, the per-frame diff, frame numbers + resend + slow refresh,
   the client applying it at vblank, and vblank-locked pacing. Start with a 2D-only game with a
   busy touch screen to prove scroll/sprites/palette changes, then add per-line registers.
4. **Pixel fallback (Plan A)** for 3D or capture on the bottom screen, switched automatically,
   plus local ink echo for touch and the SD-card block cache (section 5).
5. **3DS native client** over normal Wi-Fi using Plan A at 60 fps (optional; a 3DS in DS mode
   already gets mirroring through the .nds client).
6. Extras: mic, lid close = pause, per-game overrides (which screen goes where, swap button),
   a dash entry that launches the game and pushes the client in one step.

Steps 1-3 give a native-looking second screen for 2D touch-screen games; step 4 covers the
rest.
