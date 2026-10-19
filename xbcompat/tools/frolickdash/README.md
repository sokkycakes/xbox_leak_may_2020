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
- `nodes/FrolickWorld.cpp` – the rest of Frolick's world: `XtxTexture` (a
  Texture node from a `.xtx`, for the Background's backdrop), `Ribbon` (the
  open tube with the gradient-alpha material of Frolick's ring and pad
  bumpers) and `Dome` (the "atmo" hemisphere, multiplied over the backdrop
  with its slowly scrolling light stripes).
- `dash/` – the data: `pads/*.xui` are Frolick's own panels (`catalog`,
  `library`, the `frolick_skin` FrolickTile visual, the carousel scenes);
  `tools/genhub.py` generates `default.xap` (the hub: the backdrop, the
  dome and the ring from `frolick_assets/main.tscn`, three floor pads in
  Frolick's arc, each pad's panel on the world corners Frolick's own
  `pads.go` + `math3d` give it, left/right flights between pads once the
  panel's focus reaches its edge, as Frolick does), `backdrop.png` (the
  `backdrop.tres` radial gradient), `pads/settings.xui` (the catalog layout
  relabelled, like Frolick's RML settings panel) and the floor pad scenes
  `pads/pad_*.xui`.
- `tools/mkassets.py` – packs the data into `default.xip`, converting PNGs
  to the raw `.xtx` textures the panel reads.  `tools/mkglyphs.py` makes
  stand-in button glyphs when Frolick's are not at hand (`GLYPHS=dir`).

## Build and run

    tools/frolickdash/build.sh [WORK]      # WORK defaults to /tmp/xbdash
    xvfb-run -a tools/frolickdash/run.sh WORK --frames 150 --screenshot out.bmp --shot-frame 140

WORK is shared with `tools/dashbuild`, whose first run compiles the engine
with the leak's VC7 under Wine (about ten minutes); after that a rebuild
takes seconds.  The panels' font comes from the system's DejaVu Sans
(`FONT=file.ttf` to use another).  Under xbcompat the keyboard drives the
pad; `XBCOMPAT_INPUT_SCRIPT="100:right 200:a"` presses buttons at frames.

## Not there yet

The library carousel (A on the library pad), Frolick's pad textures and
labels (the hub's pads are drawn as flat shaded discs), and the woosh
sound.  Frolick's `hero.png` and `cover.png` are
missing on the PC too.
