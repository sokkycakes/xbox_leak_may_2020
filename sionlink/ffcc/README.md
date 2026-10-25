# FFCC's GBA link on a DS or 3DS

Final Fantasy Crystal Chronicles beams a client program to each GBA linked to
the GameCube: the radar map, command list, menus, shops and letters. The
[FFCC decompilation](https://github.com/zcanann/FFCC-Decomp) (CC0) has that
client as C source. This folder builds it for the DS and the 3DS and carries its
JoyBus link over Sion Link, so a DS or 3DS takes the GBA's place with a host on
the Sion playing the GameCube's side.

- `setup.py` fetches the decompilation at a pinned commit and copies the
  client into `build/cli` with the edits the port needs. Its graphics come
  from your disc (`--disc FFCC.iso`, extracted by the decompilation's own
  tools) or are blank (`--stub`). `--region usa|pal|jp`, default usa.
- `ds/` builds `ffcc-gba.nds`: the client plus `ds/source/port.c`, which
  stands in for the GBA hardware. The DS main engine shares the GBA's display
  registers, palettes and OAM, so most of the client runs untouched; DISPCNT,
  scrolling, DMA, the BIOS calls and the JOY port are emulated, and the
  240x160 picture is centered on the top screen.
- `ctr/` builds `ffcc-gba.3dsx` (`make`) and the 3DS Download Play child archive
  `ffcc-gba.cia` (`make cia`, needs `devkitARM`, `libctru`, `3ds-tools`, and
  `makerom`). `common/gbappu.c` renders the GBA's backgrounds, windows, sprites
  and blending in software at 60 fps in menus (~28 fps on the radar map) and
  fills the 3DS top screen: it draws the picture 264 pixels wide, with each
  window, line of text and sprite moved out toward the edge it is near and
  the backdrop pattern filling the gap, then shows it at 1.5x (396x240).
- `ffproto.h`: the JoyBus traffic over Sion Link. The host sends the
  commands a GameCube would put on the wire (reset, status, read, write); the
  DS/3DS applies each to its emulated JOY registers and runs the client's serial
  interrupt, as a GBA would.
- `sion/ffhost.c`: the GameCube's side of the wire: the client's handshake,
  then a queue of words for each player with the game's ACK/NAK and resend
  rules.
- `sion/demo.c`: a stand-in for the game, so every screen of the client can
  be tried without a GameCube. Each player gets a caravan member with a full
  bag, the disc's radar maps with the party, monsters and treasure moving on
  them, letters from home, a shop, a blacksmith, character creation and the
  "look at the TV" screen. It needs the disc's `gba` folder (`objdat.spt`
  and the `m*.mcd` maps), given with `--data`; none of it is in the repo.

## The demo on the Sion

`sion-dslink ffcc` starts the DS network if needed and runs `ffhost` with the
data in `/data/ffcc/gba`. Link a DS with the Sion Link app and it gets the
client.

`sion-dslink ffcc --3ds` starts the 3DS UDS network with `ffcc-gba.cia` queued
for 3DS Download Play (needs `/data/sionlink/3ds-keys.txt` on the Sion and
Luma3DS on the 3DS) and runs `ffhost`. Open Download Play on the 3DS, choose
**Nintendo 3DS**, receive the client from the Sion, and it boots straight into
the Sion Link network and links to `ffhost`.

On the DS or 3DS, SELECT switches between steering the menus (L/R pages
through them, as on a real GBA) and being a controller:

| DS button | as a controller |
|-----------|-----------------|
| D-pad     | walk the caravan around the map |
| A         | next map |
| B         | take a hit |
| L / R     | radar role: map, monsters, scouter, monsters and treasure |
| START     | next scene: field, creation, shop, blacksmith, TV, field |

Finishing or leaving a scene on the DS moves on too. Lines typed at ffhost
do the same for every DS (`help` lists them): `field`, `cmake`, `shop`,
`smith`, `tv`, `map`, `radar`, `hit`, `letter`, `menu`, `pad`, `screen N`.

Sound (the client's m4a music driver) isn't ported yet.
