# Frolick on the Xbox dashboard engine

sokky's Frolick dashboard (the Go + RmlUi + GLES2 launcher on their PC)
rebuilt as an application for the original Xbox dashboard's engine: the
leak's `private/ui/xapp` XAP runtime, compiled unchanged, plus the nodes
and data in this directory.  It boots under xbcompat like the real
dashboard (`tools/dashbuild`), as its own `xboxdash.xbe` with its own
`default.xip`.  Nothing in the leak's dashboard sources is touched.

## What is where

- `xui/` – a C++98 port of the project's Go XUI runtime (frolick-xui): the
  `.xui` XML parser, the class property tables (`xui_classes.cpp`, generated
  from `classes.json` by `tools/gen_classes.py`), scenes with skins,
  timelines, focus and navigation (`xui_scene.cpp`) and the renderer
  (`xui_draw.cpp`: figures, gradient and texture fills, images, text through
  the vendored `stb_truetype`).  It talks to the host through two small
  interfaces, `Backend` (textures and triangles) and `Resources` (files).
- `nodes/XuiPanel.cpp` – the xapp node that shows a `.xui` scene as a flat
  panel in the 3D scene.  It implements the backend on Xbox D3D8 (linear
  textures, `DrawIndexedVerticesUP`, clipping by geometry) and the
  resources on the engine's XIP file layer, and exposes the scene to XAP
  script: `Navigate`, `Press`, `Play`, `SetText`, `SetImage`, `SetFocus`,
  `GetFocus`, `SetVisible`, `SetOpacity`, `Reload`, with `OnPress(id)` and
  `OnFocus(id)` events.
- `nodes/CameraMover.cpp` – eased camera flights between Viewpoints
  (Frolick's 550 ms ease-in-out), since the engine only jumps or glides for
  a fixed second.
- `nodes/FrolickWorld.cpp` – the rest of Frolick's world, node for node
  from `frolick_assets/main.tscn`: `XtxTexture` (a Texture node from a
  `.xtx`, for the Background's backdrop), `Ribbon` (the open tube with the
  gradient-alpha material of the rail ring and the pad bumpers), `Disc`
  (the pad discs and the highlight ring), `Sprite` (Godot's billboard
  Sprite3D / Label3D: the pad emblems and labels, always facing the
  camera) and `Dome` (the "atmo" hemisphere, multiplied over the backdrop
  with its slowly scrolling light stripes).
- `dash/` – the data: `pads/*.xui` are Frolick's own panels (`catalog`,
  `library`, the `frolick_skin` FrolickTile visual, the carousel scenes);
  `tools/genhub.py` generates `default.xap` (the hub, from
  `frolick_assets/main.tscn`: the backdrop, dome and rail, the `nav` group's
  three pads with their bumpers, emblems and labels, the scene's cameras
  (`Camera3D` for the library, `cam_angle_pad` and its mirror for the
  sides), each pad's panel facing its camera, left/right flights between
  pads; the tiles inside a panel are not navigated, at sokky's request),
  `backdrop.png` (the `backdrop.tres` radial gradient), `pads/overlay.xui`
  (the screen overlay: the faded word behind the view, the A prompt, the
  footer with the wordmark) and `pads/settings.xui` (the catalog layout
  relabelled, like Frolick's RML settings panel).
- `tools/mkassets.py` – packs the data into `default.xip`, converting PNGs
  to the raw `.xtx` textures the panel reads.  The button glyphs are
  Xelu's Xbox 360 prompts in `art/xelu360/` (Start, A, B, X, Y: the same
  files Frolick's `glyph_*.png` are), or `GLYPHS=dir` for a set of your own;
  `tools/mkart.py` renders the pad labels as `main.tscn`'s Label3D nodes
  describe them and a ring for the emblems, until Frolick's `Circle.png`
  and pad textures arrive (`ART=dir`).  A on a catalog or settings tile flies the camera (750 ms, like Frolick's carousel zoom) down to `main.tscn`'s `cam_angle_padfocus`, mirrored for settings, with the panel faded out; B flies back.
- The padfocus pages, the full-screen menus the catalog and settings pads
  open, are the Xbox 360 Blades dashboard's third-level pages (the ones
  that slide the blades away and expand).  `tools/xur2xui.py` converts a
  Blades scene (XUIB v5 `.xur`, from a package run through `xurconv.py`
  first so its classes are the engine's) to the text `.xui` the runtime
  reads; the converted scenes are kept in `art/blades/` as they came out,
  so each page's ids, sizes, positions and navigation are Microsoft's.
  `tools/genpage.py` turns them into `pads/page_*.xui` dressed after
  sokky's catalog padfocus mockup and the 360's own sub-pages: the page
  spans the screen edge to edge, a darker header strip over a translucent
  slate body with the pad showing through, a legend bar below, and the
  list bars in the Blades' button style (rounded, thin light border, dark
  glassy fill, white text, the focused bar lit) in the panel's tones;
  `pads/page_skin.xui` holds those visuals, with Frolick's glyphs on the
  legends.  The Blades' lists are filled at run time by the 360 dashboard;
  here a list becomes its items as buttons stacked the way the list would
  stack them, with the texts from `genpage.py`'s PAGES table (placeholders
  until the pages read the console).  The hub shows a page on a HUD
  `XuiPanel` (`thePage`) once the camera lands on the focus view, opening
  it over 30 frames like `dashmain.xur`'s `1OpenFull`: up and down walk
  the list, A opens the item's page (a 10-frame fade, the Blades'
  FadeIn), B returns to the parent page or, on the root page, closes it as
  the camera flies back.  Pages so far: settings pad: Settings (Console
  Settings, Memory, Network Settings) > Console Settings with its meta
  panel, Storage Devices > the drive's items, Select Network
  Configuration with its meta text; catalog pad: Marketplace > Game
  Downloads, Featured Downloads, Demos and Trailers, Themes, Download
  History.  Item icons and the Marketplace banner wait for Frolick's art.

The library view is GameTap's game belt (`art/gametap/`: the decoded
`beta2scene-belt.json` and the spec `game-belt-render.md`).  `tools/mkbelt.py`
packs the 13 cover nodes gsb_0shape..gsb_12shape, each with its own cover
mesh and its frame 1000..1020 poses, into `belt.bin`; the `Belt` node in
`nodes/XuiPanel.cpp` draws each cover mesh at its sampled world matrix, in
shape-list order, depth tested.  The camera is fitted to sokky's mockup screenshot of the belt view (from
the pad labels: Frolick's carousel camera 0.19 ahead and 0.07 up, pitched
up 22 degrees), widened to 96 degrees (the hub's are 80) so the three pad
labels sit inside the NTSC title-safe area; the CameraMover lerps the
field of view along with the flight.  The belt's ring is the rail ring,
its axis and height, the covers at 0.95 of the ribbon radius at GameTap's
sizes, and the focused slot is pulled 5.6 off the wall toward the camera
(sokky's pick, to keep the focused tile large); a cover sliding in or out
follows.  The two slots either side of the focus sit behind the pulled-in
cover from this camera, so they hold nothing (skip 2): the previous and
next game are on the first slots past them, the rest follow outward slot
by slot, and the belt beyond stays contiguous.  On a step the game
entering or leaving the focus crosses the empty slots within the step
time, cover pose to cover pose, while every other game moves one slot.
Motion is
the sampled clip only: left and right play Belt_Right / Belt_Left (20 frames
at speed 2.5, 0.267 s, input dropped while a step plays, as the client's lock
does); when a step ends the covers are back on their frame-1000 slots and
each slot takes the next game.  A on the library panel flies the camera in
(750 ms, the panel fades, the covers fade in); B flies back.  Each cover wears
Frolick's tile art, `carousel_card.xui` draped over the cover mesh at the
card's own aspect, and the focused cover the `carousel_focus.xui` overlay
(arrows, the lime A capsule over its bottom-right corner, the title).  In
the belt view the hub's A prompt and chevron are hidden and the B/back hint
sits in the screen overlay above the library label, placed as in sokky's
mockup.  The two tiles at the left end are
reserved: "All games" (grid mode, not built yet) and the game in the disc
tray or game card, each with an icon centred on its tile (`Belt` `icons`;
the icon images themselves are not in the cloud yet).  The belt opens on
the first game.  Not there yet: the icons, real cover art, grid mode, the
disc tile's empty and loaded states, the step sounds, and what the list
ends do (nine placeholder games, no wrap).

## Build and run

    tools/frolickdash/build.sh [WORK]      # WORK defaults to /tmp/xbdash
    xvfb-run -a tools/frolickdash/run.sh WORK --frames 150 --screenshot out.bmp --shot-frame 140

WORK is shared with `tools/dashbuild`, whose first run compiles the engine
with the leak's VC7 under Wine (about ten minutes); after that a rebuild
takes seconds.  The panels' font comes from the system's DejaVu Sans
(`FONT=file.ttf` to use another).  Under xbcompat the keyboard drives the
pad; `XBCOMPAT_INPUT_SCRIPT="100:right 200:a"` presses buttons at frames.

## Not there yet

The belt's game cover art, Frolick's pad textures
(`_albedo003..005`, tiled 2x2 on the discs) and its `Circle.png`, and the
woosh sound.  Frolick's `hero.png` and `cover.png` are
missing on the PC too.
