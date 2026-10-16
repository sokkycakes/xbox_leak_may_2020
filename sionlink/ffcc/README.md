# FFCC's GBA link on a DS

Final Fantasy Crystal Chronicles beams a client program to each GBA linked to
the GameCube: the radar map, command list, menus, shops and letters. The
[FFCC decompilation](https://github.com/zcanann/FFCC-Decomp) (CC0) has that
client as C source. This folder builds it for the DS and carries its JoyBus
link over Sion Link, so a DS takes the GBA's place with a host on the Sion
playing the GameCube's side.

- `setup.py` fetches the decompilation at a pinned commit and copies the
  client into `build/cli` with the edits the port needs. Its graphics come
  from your disc (`--disc FFCC.iso`, extracted by the decompilation's own
  tools) or are blank (`--stub`). `--region usa|pal|jp`, default usa.
- `ds/` builds `ffcc-gba.nds`: the client plus `ds/source/port.c`, which
  stands in for the GBA hardware. The DS main engine shares the GBA's display
  registers, palettes and OAM, so most of the client runs untouched; DISPCNT,
  scrolling, DMA, the BIOS calls and the JOY port are emulated, and the
  240x160 picture is centered on the top screen.
- `ffproto.h`: the JoyBus traffic over Sion Link. The host sends the
  commands a GameCube would put on the wire (reset, status, read, write); the
  DS applies each to its emulated JOY registers and runs the client's serial
  interrupt, as a GBA would.
- `sion/ffhost.c`: the GameCube's side. For now it runs the client's
  handshake and reads what the client sends (its buttons and screen); it
  doesn't play the game.

Sound (the client's m4a music driver) isn't ported yet.
