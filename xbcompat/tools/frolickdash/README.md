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
  pads once the panel's focus reaches its edge, as Frolick does),
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
  and pad textures arrive (`ART=dir`).  A on a catalog or settings tile flies the camera (750 ms, like Frolick's carousel zoom) down to `main.tscn`'s `cam_angle_padfocus`, mirrored for settings, with the panel faded out; B flies back.  The expanded menu that belongs there is not built yet.

## Build and run

    tools/frolickdash/build.sh [WORK]      # WORK defaults to /tmp/xbdash
    xvfb-run -a tools/frolickdash/run.sh WORK --frames 150 --screenshot out.bmp --shot-frame 140

WORK is shared with `tools/dashbuild`, whose first run compiles the engine
with the leak's VC7 under Wine (about ten minutes); after that a rebuild
takes seconds.  The panels' font comes from the system's DejaVu Sans
(`FONT=file.ttf` to use another).  Under xbcompat the keyboard drives the
pad; `XBCOMPAT_INPUT_SCRIPT="100:right 200:a"` presses buttons at frames.

## Not there yet

The library carousel (A on the library pad), Frolick's pad textures
(`_albedo003..005`, tiled 2x2 on the discs) and its `Circle.png`, and the
woosh sound.  Frolick's `hero.png` and `cover.png` are
missing on the PC too.
