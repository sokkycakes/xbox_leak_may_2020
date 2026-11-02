# Controller Configuration: implementation strategy

Written 2026-11-02 for the Guide's **Now Playing > Controller Configuration**
row (`xbcompat/src/guide/guide.cpp`, `btnViewAchievements`, "not here yet").
The goal is per-game controller remapping in the style of Steam Input, for
Xbox titles under xbcompat and for Windows game cards under kazeta-guide.

Background research (Steam 2014 v1 format, Steam Input v3, sc-controller) is
in the thread "Research Steam Input for controller config" and summarised in
the write-up linked from the project memory. This document only decides how we
build ours.

## 1. What we have today

| Host | Reads pads | Hands the game | Where the state is built |
|---|---|---|---|
| xbcompat (Xbox titles) | SDL2 `SDL_GameController`, one per XInput port | `XINPUT_GAMEPAD` through the HLE'd XInput API | `read_controller()` -> `read_port()` -> `sample()` in `xbcompat/src/hle/xinput.c` |
| kazeta-guide (Windows carts) | SDL2 `SDL_GameController`, up to 4, merged | one uinput Xbox 360 pad (`/dev/uinput`) | `ui_forward()` in `bootani/sion/xbox/push-wincart/kazeta-guide/host.cpp` |

Both hosts already:

- run SDL 2.32.10 with HIDAPI/libusb (`tools/sion/sdlpads`), so SDL's own
  drivers give us the standard layout for Xbox, PlayStation, Switch, 8BitDo
  and N64 adapters, plus paddles, touchpads and gyro where the pad has them;
- know the title: `xbe_title_id()` in xbcompat, `KAZETA_TITLE_ID` in
  kazeta-guide;
- mask the pad while the Guide is open (`xinput_guide_mask()`, `masked_`), and
  the Guide draws on the same XUI runtime in both;
- forward rumble back to the real pad.

What neither has: any per-game remapping. A pad's SDL mapping string
(`gamecontrollerdb.txt`, `map_original_pad()`) decides which raw button is
"A", and that is the end of it.

## 2. The three ways to do it

### A. SDL only

Install a per-title `SDL_GameControllerAddMapping()` string at launch so SDL
itself reports, say, B where the pad has A.

- For: no new code to speak of; works in both hosts unchanged.
- Against: a mapping is keyed by controller GUID, so it changes every pad of
  that model, not this game's layout. It only renames raw buttons and axes
  1:1. It cannot turn a stick into a D-pad, invert an axis, add a deadzone or
  response curve, do hold/double-tap/turbo, mode shift, gyro aiming, or emit
  keyboard and mouse for Windows games. Changing it needs a close and reopen,
  which the title sees as a pad removal and insertion. SDL2 has no deadzone or
  curve support at all.

Verdict: not a remapping layer. SDL stays what it already is, the device
layer that normalises hardware into the standard controller, and it stays the
only place that knows about raw buttons.

### B. Steam's way: in the process that fakes the pad

Steam does its remapping inside the Steam client, which also presents the
fake XInput pad to the game. Its data model (v3) is: a physical **source**
(left stick, right pad, face buttons, triggers, gyro) is bound to a **group**
with a **mode** (joystick, dpad, four buttons, mouse, radial menu, ...);
each input in the group has **activators** (full press, long press, double
press, start, release, chord) that carry **bindings** and options (turbo,
toggle, haptics); a **preset** is a whole layout (an action set); a
**mode shift** binds a second group to the same source while a button is
held.

- For: our `xinput.c` *is* the fake XInput, and kazeta-guide *is* the fake
  360 pad, so the hook points exist. The title ID is known in-process. The
  Guide that edits the profile is in the same process. No IPC, no extra
  process at boot, no latency added beyond the per-sample evaluation. The v3
  model is the one users recognise and the one templates and the UI map onto
  naturally.
- Against: the code is written twice unless it is a library shared by both
  hosts (it will be). Keyboard and mouse output is only meaningful for
  Windows carts.

### C. sc-controller's way: a daemon and virtual devices

A separate daemon owns the pads, evaluates a per-input tree of composable
actions (`hold(...)`, `mode(LB, ...)`, `sens(1.2, XY(...))`) and writes to
uinput devices: a virtual gamepad, keyboard and mouse. Per-game switching
matches the focused X11 window; a GTK GUI talks to the daemon over a socket.

- For: one daemon serves every client; keyboard and mouse output come for
  free; the composable action tree is the cleanest evaluation model found.
- Against, on the Sion: xbcompat would read the virtual pad back through SDL,
  a second hop and a second 8 ms poll interval on every input. Rumble would
  have to travel game -> xbcompat -> uinput FF -> daemon -> real pad.
  Profile selection by window title does not exist on KMS/DRM; it would need
  IPC from the host that knows the title. The Guide would need IPC to edit
  live. It is another process in the RAM-root boot, with its own crash and
  update story. Python is not on the image, and the reference code is GPL,
  so it would be a rewrite in C anyway. The merging rule kazeta-guide uses
  (any pad drives the one virtual pad) would move into the daemon.

Verdict: the right shape for a desktop with many clients, the wrong shape
for a console where exactly one title runs and the Guide is already inside
it.

## 3. Decision

**Build B with C's evaluation model: one small in-process C library,
`padmap`, with Steam Input's data model and sc-controller's composable
node evaluation. SDL stays the device layer underneath it.**

Why this and not the others, in one line each:

- Not A, because SDL mappings are per pad model and 1:1; they cannot express
  anything a user would open the Controller Configuration page for.
- Not C, because every reason for a daemon (many clients, window-based
  switching, process isolation) is absent on the Sion, and every cost
  (double hop, rumble round trip, IPC, another boot process) is real.
- Steam's data model, because it is what the Guide page has to show anyway:
  "this stick is a D-pad", "hold LB to shift", "this preset is Menus".
- sc-controller's evaluation, because a node with the same four callbacks
  (`press`, `release`, `move`, `tick`) makes every mode and activator
  compose without special cases, and it is a few hundred lines of C.

Licence: `padmap` is written from scratch under xbcompat's own licence (not
GPL). sc-controller is GPL-2 and stays a read-only reference; nothing is
copied. Its default profiles and menus are CC0 and may be borrowed as data.
Valve's VDF files are a format reference only and are not shipped.

## 4. Design

### 4.1 Where it sits

```
real pad --SDL (HIDAPI, gamecontrollerdb)--> standard controller state
       --padmap (profile for this title, this pad type)--> virtual report
       --sink--> XINPUT_GAMEPAD (xbcompat)   or   uinput pad + kbd + mouse (kazeta-guide)
```

- **xbcompat**: `read_controller()` keeps filling a plain `XINPUT_GAMEPAD`
  for the no-profile case. With a profile, `read_port()` calls
  `padmap_sample(port, &sdl_state, &report)` instead and copies
  `report.xbox` into the `XINPUT_GAMEPAD`. The keyboard pad, the input
  script, the pipe and `guest_view()` masking are untouched: padmap runs
  before the merge, per real pad, so the Guide mask still zeroes the title's
  view and the reset combo still reads the mapped pad.
- **kazeta-guide**: `ui_forward()` calls padmap per pad before its "any pad
  drives the pad" merge, then also flushes `report.keys[]` and
  `report.mouse` to two more uinput devices (keyboard, mouse) opened next to
  the pad in `ui_open()`.
- The Guide button is never remappable. L+R+Back+Start reset stays on the
  mapped output so it keeps working under a swapped layout.

### 4.2 Source state (input to padmap)

One struct per real pad, filled from SDL every sample:

```
struct padmap_src {
    uint32_t buttons;     /* SDL_CONTROLLER_BUTTON_* bits incl. paddles, misc, touchpad */
    int16_t  lx, ly, rx, ry;
    int16_t  lt, rt;
    struct { bool down; float x, y; } touch[2];   /* DS4/DS5/Deck-style pads */
    float    gyro[3], accel[3];                   /* rad/s, m/s^2; valid flag */
    bool     has_gyro;
    SDL_GameControllerType type;
};
```

### 4.3 Virtual report (output of padmap)

```
struct padmap_out {
    /* always */
    uint16_t xbuttons; uint8_t a,b,x,y,black,white,lt,rt; int16_t lx,ly,rx,ry;
    /* Windows carts only */
    struct { uint16_t key; bool down; } keys[16]; int nkeys;
    struct { int dx, dy, wheel; uint8_t buttons; } mouse;
    /* haptics requested by nodes (hold confirm, ring edge) */
    uint16_t rumble_lo, rumble_hi, rumble_ms;
};
```

Black/White are the Xbox shoulders; on the 360-style uinput pad they are
TL/TR. The sink does that translation, not the profile.

### 4.4 Profile model (Steam v3 shape, our JSON)

```json
{
  "version": 1,
  "title": "Standard",
  "controller_type": "any",
  "presets": [
    { "id": 0, "name": "Default",
      "sources": {
        "left_stick":  { "mode": "joystick", "output": "left_stick", "deadzone": 0.12, "curve": "linear", "invert_y": false },
        "right_stick": { "mode": "joystick", "output": "right_stick", "sensitivity": 1.0 },
        "dpad":        { "mode": "dpad", "inputs": { "up": {"press": "dpad_up"}, "down": {"press": "dpad_down"},
                                                     "left": {"press": "dpad_left"}, "right": {"press": "dpad_right"} } },
        "face":        { "mode": "buttons", "inputs": {
                           "a": { "press": "a" },
                           "b": { "press": "b", "long_press": "back", "long_ms": 400 },
                           "x": { "press": "x", "turbo_hz": 10 },
                           "y": { "press": "y" } } },
        "shoulders":   { "mode": "buttons", "inputs": { "lb": {"press": "white"}, "rb": {"press": "black"} } },
        "triggers":    { "mode": "trigger", "inputs": { "lt": {"analog": "lt"}, "rt": {"analog": "rt", "full_press": "a"} } },
        "gyro":        { "mode": "off" }
      },
      "mode_shifts": [
        { "hold": "lb", "sources": { "right_stick": { "mode": "dpad", "inputs": { "up": {"press":"y"} } } } }
      ]
    }
  ],
  "preset_switch": { "button": "back", "mode": "cycle" }
}
```

- `mode` per source: `joystick`, `dpad`, `buttons`, `trigger`,
  `mouse` (Windows only), `scroll` (Windows only), `radial_menu` (later),
  `off`.
- `inputs` are the group's own names (`up`, `a`, `lt`, `click`, `edge`).
  Each has activators: `press`, `long_press`, `double_press`, `release`,
  `start_press`, with options `long_ms`, `turbo_hz`, `toggle`.
- A binding string names an **output**: `a` .. `rthumb`, `dpad_*`, `start`,
  `back`, `left_stick`, `right_stick`, `lt`, `rt`, or `key:w`, `mouse:left`,
  `mouse:move`, `wheel`, or `preset:Menus`, or `none`.
- `mode_shifts` override whole sources while a button is held, exactly
  Steam's second group on the same source.
- `presets` are action sets. One preset is enough for almost every Xbox
  title; the field exists so Windows games with menu/drive/walk layouts fit.

The JSON is hand-readable and small enough for a few hundred lines of
parser with no dependency (xbcompat has only `stb_image.h` in
`third_party`; a tiny recursive-descent JSON reader is written for this).

### 4.5 Evaluation (sc-controller shape)

Every mode and every activator is a node:

```
struct padmap_node {
    void (*press)(struct padmap_node*, struct padmap_ctx*);
    void (*release)(struct padmap_node*, struct padmap_ctx*);
    void (*move)(struct padmap_node*, struct padmap_ctx*, float x, float y);  /* sticks, pads, triggers, gyro */
    void (*tick)(struct padmap_node*, struct padmap_ctx*, uint64_t now_us);   /* long press, turbo, double tap */
    struct padmap_node *child[8];
};
```

`padmap_sample()` diffs the button bits against the previous sample and
calls `press`/`release` on the bound node (sc-controller's XOR loop), calls
`move` on sources whose position changed, then `tick` on the nodes that asked
for timers, then writes the collected outputs into `padmap_out`. Mode shift is
a node that routes to one of two children depending on a held button. Turbo,
long press and double press are wrapper nodes around the binding they
decorate. This is the whole runtime; it runs in `sample()` at the title's
8 ms poll interval and touches no heap.

### 4.6 Profile storage and selection

```
/opt/xbcompat/share/padmap/templates/*.json        shipped: Standard, Southpaw, Legacy (swap sticks),
                                                    Gyro aim, Windows: Keyboard+mouse
/data/xbox/input/<titleid>/selected                 name of the active profile, or absent
/data/xbox/input/<titleid>/<name>.json              the title's own profiles
/data/xbox/input/default-<controller_type>.json     per pad family, used when a title has none
```

Lookup order at launch and at every pad insertion: title + controller type,
title, controller-type default, built-in passthrough. `<titleid>` is the XBE
title ID (as the UDATA save folders) or the cart's `KAZETA_TITLE_ID`. The
Guide edits the profile in memory and `padmap_swap()` installs it atomically
when the Guide closes (`xinput_guide_mask(false)`), so a test run of a new
layout never races the title.

### 4.7 Guide page

The Blades list page already used for Now Playing, on sokky's slate panel:

1. **Controller Configuration** (list): Template, Reset, then one row per
   source (Left Stick: Joystick, Right Stick: Joystick, D-pad, A/B/X/Y,
   LB/RB, LT/RT, Gyro: Off, Paddles) showing its mode and, for buttons, the
   binding. LT/RT switch preset when more than one exists. B asks Save /
   Discard, like Steam's `controller_bindings_save`.
2. **Source page**: mode picker first (Joystick, D-pad, Buttons, Mouse, Off),
   then one row per input in that mode with its binding, and tuning rows
   (deadzone, sensitivity, invert) as sliders in the Guide's existing slider
   style.
3. **Binding chooser**: the Xbox outputs as a grid; for Windows carts a second
   tab with keys (on-screen keyboard reused from the Guide) and mouse.
4. **Advanced** (Y on an input row): long press, double press, turbo, toggle,
   mode shift (pick the hold button).

Live preview: while the page is open the Guide owns the pads, so a "Test"
row can show the mapped output as a drawn pad without the title seeing it.

## 5. Phases

Each phase ends with something testable in the cloud, and a kit only when it
changes what sokky sees. No Sion installs before phase 1 ships.

| Phase | Deliverable | Test |
|---|---|---|
| 0 | `xbcompat/src/input/padmap.{c,h}`: JSON profile, sources `joystick`/`dpad`/`buttons`/`trigger`, deadzone, invert, swap; hooked into `read_port()`; env `XBCOMPAT_PADMAP=<file>` to force a profile | `tests/padmap_test.c` with `XBCOMPAT_VIRTUAL_PAD` runs: a southpaw profile moves the right stick when the virtual left stick is pushed, etc. |
| 1 | Guide page 1 + 2 + chooser, templates, save/select under `/data/xbox/input`; same library compiled into kazeta-guide (Xbox outputs only) | Guide host test under Wine+Xvfb (the existing XUI test path); then a kit |
| 2 | Activators: long press, double press, turbo, toggle; mode shift; presets with a switch button | padmap_test timing cases; a Windows cart with a "Menus" preset |
| 3 | Gyro to right stick (SDL sensors), touchpad as stick or buttons, paddles, per-controller-type defaults | DS4/8BitDo Pro on the Sion once phase 1 is in |
| 4 | Windows carts: uinput keyboard + mouse sinks, `mouse`/`scroll` modes, Keyboard+mouse template; radial menu drawn by the Guide overlay | Silent Hill 4 with a keyboard-and-mouse layout |

Phase 0 is roughly 600 lines of C plus 200 of tests; phase 1 is Guide work
in `guide.cpp` and new XUI scenes; the rest are additions to the node set.

## 6. Risks and how they are handled

- **Latency**: evaluation is a fixed loop over at most ~40 nodes per sample,
  no allocation; it is well under the 8 ms XInput interval and the 1 ms the
  title's poll already costs.
- **Guide mask**: padmap runs before `guest_view()`, so the Guide's masking
  and the post-close idle latch keep working. A profile swap only happens at
  mask release.
- **Keyboard pad and scripts**: they bypass padmap on purpose; headless test
  runs stay deterministic.
- **Pad hot-plug**: profile lookup reruns in `sync_devices()` so a different
  pad type picks its own default.
- **Reset combo and Guide button**: read from the mapped output and from the
  raw Guide bit respectively; neither can be remapped away.
- **Two hosts, one library**: padmap has no SDL or OS calls of its own; the
  host fills `padmap_src` and consumes `padmap_out`, so both build it from
  the same file and the sinks differ.
- **Settings file clash**: profiles live under `/data/xbox/input`, not in
  `<hdd>/.guide`, so volume/vibration settings are untouched.
- **Scope creep**: radial menus, action sets for Xbox titles and OSD
  keyboards are phase 4 or later; phase 0 and 1 are what the Controller
  Configuration row needs to stop saying "not here yet".
