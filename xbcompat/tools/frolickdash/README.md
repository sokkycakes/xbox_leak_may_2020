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
  `pads/library.xui` is written by `tools/genlibrary.py` from sokky's
  Figma frame of the library pad (file fL710Dg0cpzZzYe0OZSBVu, node
  73:88): one XuiFigure per Figma fill layer, Figma's vertical gradients
  as linear fills at Rotation -90, its affine radial fills as the brush's
  Rotation, Scale and Translation (an ellipse is a rotated, axis-scaled
  circle, whatever the matrix's skew; the script checks the brush against
  the ellipse), text in Fira Sans at PointSize = Figma px * 0.75 with the
  box top where Figma has it, and the tiles' focus visuals
  (`FrolickFrameL/TR/BR`, FrolickTile cut to each tile's corners, written
  into the skin) carrying only the halo, glow and shine, since the Figma
  layers draw the borders and gloss.  A text element's `<Font>` name picks
  `Name.ttf` (spaces dropped) from the XIP, `FiraSans.ttf` from
  `art/fonts/` here; a name with no file draws in the default font;
  `tools/genhub.py` generates `default.xap` (the hub, from
  `frolick_assets/main.tscn`: the backdrop, dome and rail, the `nav` group's
  three pads with their bumpers, emblems and labels, the scene's cameras
  (`Camera3D` for the library, `cam_angle_pad` and its mirror for the
  sides), each pad's panel facing its camera, left/right flights between
  pads; the tiles inside a panel are not navigated, at sokky's request),
  `backdrop.png` (the `backdrop.tres` radial gradient), `pads/overlay.xui`
  (the screen overlay: the faded word behind the view, the A pin over the focused pad (the Figma's `A_Focus`: its pin shape as a figure with the Xelu A in the head), the
  footer with the wordmark) and `pads/settings.xui` (the catalog layout
  relabelled, like Frolick's RML settings panel).
- `tools/mkassets.py` – packs the data into `default.xip`, converting PNGs
  to the raw `.xtx` textures the panel reads.  The button glyphs are
  Xelu's Xbox 360 prompts in `art/xelu360/` (Start, A, B, X, Y: the same
  files Frolick's `glyph_*.png` are), or `GLYPHS=dir` for a set of your own;
  `tools/mkart.py` renders the pad labels as `main.tscn`'s Label3D nodes
  describe them and a ring for the emblems, until Frolick's `Circle.png`
  and pad textures arrive (`ART=dir`).  A on a catalog or settings tile flies the camera (750 ms, like Frolick's carousel zoom) down to `main.tscn`'s `cam_angle_padfocus`, mirrored for settings, with the panel faded out; B flies back.
- Frolick also runs as an installed game: `build.sh` writes `game/default.xbe`
  (title ID 46524C4B "Frolick") from the same link, and the XBE finds its
  XIP, scenes and fonts beside itself on D: when `d:\default.xip` exists
  (`E:\Games\Frolick` on a console) and on the dashboard partition Y:
  otherwise, so one image serves both installs (`build.sh` compiles its
  own `main.cpp` from dashbuild's patched copy for that).  The real
  dashboard lists it in Games and Play Game launches it; `dist/game` is
  the Sion kit (XBE, XIP, HLE map, install script that copies the engine's
  fonts beside the XBE), `dist/sion` the dashboard-replacement kit.
- The padfocus pages, the full-screen menus the catalog and settings pads
  open, are the Xbox 360 Blades dashboard's third-level pages (the ones
  that slide the blades away and expand).  `tools/xur2xui.py` converts a
  Blades scene (XUIB v5 `.xur`, from a package run through `xurconv.py`
  first so its classes are the engine's) to the text `.xui` the runtime
  reads; the converted scenes are kept in `art/blades/` as they came out,
  so each page's ids, sizes, positions and navigation are Microsoft's.
  `tools/genpage.py` turns them into `pads/page_*.xui` laid out after
  sokky's catalog padfocus mockup: the page spans the screen edge to
  edge, a translucent slate body with the pad showing through; the
  header above it and the legend bar below have no fill, so the pad's
  big label stays on screen as the page's title, sliding into the frame
  with the zoom so the whole word shows (`LABEL_FOCUS_SHIFT` in
  `genhub.py`, output pixels turned into a world offset the `Sprite`
  node eases to; the camera stays sokky's), the page's own header is its
  sub-title resting on the body (right of the label on the catalog's
  pages), the body has 6 output lines of padding above its content and
  below it before the legends, which sit on the pad; body and meta text
  are white.  The page quad is scaled in about its centre
  (`PAGE_SAFE` in `genhub.py`, 0.93) so every page element, the A and X
  legends included, sits inside NTSC action safe (the inner 90%); the
  pad's label is not scaled with it.  Item text is white at rest and the
  Blades' dark on the highlighted row: `genpage.py` keys every button
  and list-item visual's text presenters so in the page skin, and the
  pages feed their list lines through the visuals' own presenters
  (`Data<k>` on the item, the slot its presenter's `DataAssociation`
  names, as the dashboard's lists do).  Everything on the page is drawn
  by the Blades' own skin: `art/blades/skin.xui` is the subset of
  `dash_dashuisk.xzp`'s `skin.xur` the pages use (`tools/skinsubset.py`
  pulls named visuals out of the converted skin), with its button PNGs
  beside it, so the buttons, highlights, legends, meta pane, list items
  and progress bar are the 360's visuals as XuiTool made them (dark text
  with a light shadow, a white glow on the focused row, lines between
  rows).  The Blades' lists are filled at run time by the 360 dashboard;
  here a list becomes its items as buttons with the list's item visual,
  stacked the way the list would stack them, their bound lines drawn as
  texts in the presenters' slots, with the texts from `genpage.py`'s
  PAGES table (placeholders until the pages read the console).  The meta
  pane gets its text through the control's data slots (`SetData(id, k,
  text)` fills `Data<k>`, which a presenter with `DataAssociation` k
  shows, as the 360 binds list and meta data).  Runtime details the skin
  needed: a visual with no Normal frame rests at frame 0 when a Stop
  command sits there; a progress bar's visual is held at the frame its
  Value picks between Normal and EndNormal; a fill's Translation is a
  fraction of the figure's box and its Scale multiplies the box into
  brush space (0.4 stretches a gradient over 2.5 boxes), as XuiTool
  writes them and the 360 draws them (checked against the Blades
  running in the GFWL XUI engine host, `/mnt/project-files/xui360`, which
  renders the same scenes), the brush rotating first and translating in
  the rotated frame (the Metapane's top corners are 90-degree-rotated
  radial fills whose centres only land on the inner corner that way);
  PointSize is in points, the em 4/3 PointSize pixels, with the baseline
  the font's ascent below the box's top (the engine's legend and header
  glyphs and sokky's real-console capture measure so; Frolick's own
  scenes, authored as pixel heights, carry converted sizes); a control
  with Enabled false rests in its visual's NormalDisable state (the
  greyed Y and X legends).  The list items' icons are the dump's own
  `ico_64x_*.png`, pulled from the packages into `art/blades/`; the pad's
  big word and the overlay's faded word and wordmark hide while a page is
  open.  The hub shows a page on a HUD `XuiPanel` (`thePage`) once the
  camera lands on the focus view; the panel is wider than the HUD and
  centred on canvas row 373.7 (`viewY`) so the Sion's 720x480 shows the
  window a 360 shows at 16:9, rows 58.7 to 688.7 of the 1120x770 canvas,
  at the engine's scale with 66 output pixels cropped each side.  The
  page opens over 30 frames like `dashmain.xur`'s `1OpenFull`: up and down walk
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
the belt view the hub's A pin is hidden and the B/back hint
sits in the screen overlay above the library label, placed as in sokky's
mockup.  The two tiles at the left end are
reserved: "All games" (grid mode, not built yet) and the game in the disc
tray or game card, each with an icon centred on its tile (`Belt` `icons`;
the icon images themselves are not in the cloud yet).  The belt opens on
the first game.  Not there yet: the icons, real cover art, grid mode, the
step sounds, and what the list ends do (no wrap).

The console's data behind the views (2026-10-20, sokky: "dashboard
functionality added back into it", starting with game data management
and launching games) comes from the dashboard's own code, compiled in
unchanged, and only the script and two small nodes are Frolick's:

  The tiles' buttons work from the hub (sokky 2026-10-20: the glyph on a
  tile is the button that opens it; the press first flies to the pad's
  focus view, then opens the page; `HubButton` in genhub.py).  Library:
  Start launches the disc or game card at once, X opens Recent Games
  (`page_recent`, the games launched most recently: GameCollection
  writes each launch's key to T:\recent.txt and `GetRecentCount` /
  `GetRecentGame` list the scanned games those keys name, newest first;
  A or Start plays one), Y opens Favorite Games (`page_favorites`, an
  empty Blades list until favorites exist).  Catalog: Start the
  Marketplace, X (spotlight) its hot list, Y (downloads) its download
  history.  Settings: Start the settings menu, X (quick panel) and Y
  (video) the console settings with Display first.  The Blades scenes
  stand in until the real pages exist.  The library's label hides on
  its pages (the belt camera leaves it small and far off), the pages'
  own header serving as the title.
  The library pad's own game tile shows the disc or game card that is
  in (cover, name, publisher, Start; the publisher is the title ID's
  two-letter code named through the XboxDev wiki's table in
  GameCollection.cpp, the bare code when the table has no name, a cart's
  Publisher= or Developer= .kzi key, since an XBE carries no developer
  name) and, when neither is present, sokky's
  no-media variant of the panel (Figma 143:47: "Insert a Game Disc or
  Game Card." with the Material Symbols disc and card icons,
  art/figma/nodisc_*.svg rasterised by tools/svgicon.py); the hub's
  UpdateLibraryTile switches the two sets of controls at start-up and on
  OnMediaChanged.
- The belt lists the console's games: `GameCollection` (dashbuild's
  node, the Games screen's list) scans the disc tray or game card slots
  and `E:\Games` when the belt opens (`RefreshBelt`, and again when a
  card comes or goes); tile 0 stays the grid, tile 1 is the tray (its
  game, or "No Disc"), the games follow.  Each cover carries the game's
  title image out of its XBE (`GetGameImage`, cached in `T:\GameImages`),
  drawn across the card (`pads/carousel_cover.xui`, set by
  `Belt.SetGameImage`; the grid and tray tiles keep their small icons).
  Start on a cover launches it the way the Xbox dashboard launches a
  game (`LaunchGameWithLogo`, as default.xap's theLauncherLevel): the
  launch Level's backdrop, the dashboard's own xboxlogo.bmp on black,
  binds at once, the launch sound starts (`theLaunchSound`, an
  AudioClip on launch.wav, sokky's pick in art/sounds, a loose file
  beside the XBE as the dashboard keeps its own sounds) and the hub
  cross-fades out over it in the Level's 0.75 s (every node of the hub
  multiplies by `g_nEffectAlpha`; the ribbon, discs and dome fade through
  the texture factor), the logo holds until the sound has finished
  (`OnEndOfAudio`; at once if the sound could not start, after 10 s if
  it never reports its end), then `LaunchGame` keeps the frame
  (`PersistDisplay`), D: becomes the game's directory and the console
  reboots into its XBE; when Frolick runs as a game, the game comes back
  to the dashboard that launched Frolick, not to Frolick.  Play Game on
  the game page takes the same route.  Start reaches the script because
  dashbuild patches the engine's Joystick.cpp to send OnStartDown and
  OnBackDown (falling back to OnADown/OnBDown for a script without
  them, so the stock dashboard is unchanged); the hub Level is
  `unloadable false`, as it has no archive to drop when it is left.  A opens the game's page over the belt: the Blades' Game
  Summary scene (`art/blades/401_GameDetails.xui`, `page_game` in
  genpage.py) as the Xbox dashboard's Games pod shows a game, with Play
  Game, Game Storage (the title's saved games page from Settings >
  Memory, B climbs back out through its parents) and, for a disc or card
  game not on the hard disk, Install Game (`GameCollection.StartInstall`,
  the header shows the copy's progress, a notice when it is done and the
  belt rescans).  The meta pane carries the title image, the platform
  and runtime as a Kazeta cart names them (`GetGamePlatform`,
  `GetGameRuntime`: an Xbox game is Xbox on xbcompat; a cart's `Runtime`
  key names the platform, and its `.kzr` image on the card, found the way
  Kazeta looks for it, the runtime), the saved game count and where the
  game is.
- The hub's A pin (sokky, 2026-10-20): `XuiPanel.Fade(id, opacity, s)` and
  `Pulse(id, dy, period, s)` animate overlay controls from script.  The
  pin fades out (0.15 s) the moment A lands on a pad or a pad flight
  starts, and back in (0.2 s) when the camera settles on a pad (the
  movers' OnArrive); it never shows in the belt or on a page.  Idle it
  dips 5 HUD px every 2 s over 0.6 s, a half sine on an eased-out clock
  (PIN_* in genhub.py).
- Settings > Memory are the dashboard's Memory screen on the Blades pages:
  `nodes/Storage.cpp` hands script the engine's `CTitleArray` (the titles
  under a device's UDATA/TDATA with their saves, downloads and installed
  games, dashbuild's patched list), the saves' SaveMeta names, dates and
  block counts, their images, and runs copies through the engine's
  `CGameCopier` and deletes on a thread as `SavedGameGrid.cpp` does
  (events OnCopyProgress / OnCopyComplete / OnCopyError / OnDeleteEnd).
  A `MemoryMonitor` node mounts memory units as they come.  The pages
  (`genpage.py` PAGES, `dynamic`) are the Blades' list scenes with as many
  rows as their box holds; the hub script fills the rows from the node
  (`MEMORY_SCRIPT` in `genhub.py`: devices with their free space and a
  gauge, titles with icon and "N saved games, N blocks", saves with icon,
  date and size) and scrolls when the focus runs off an end.  A on a save
  opens Copy / Move / Delete (copy and move want a second device), Y on a
  title deletes all its data, Y on a save deletes it; every delete and
  overwrite asks first, as the Memory screen does, and the page's
  sub-title shows the copy's progress.
- Icons: an `ImagePath` ending in `.xbx` (a save's SaveImage.xbx, a
  title's TitleImage.xbx, a cached title image) is read by the engine's
  own texture parser (`Image.cpp ParseTexture`, `Backend::native` in the
  XUI runtime), so the pages show the console's images as they are.
- `tools/mkteststage.sh WORK` stages a test console under `WORK/frolick/run`
  for the cloud: three games built from Frolick's image with their own
  title images, one of them in the disc tray, and saved games for two of
  them and for a title with no game.

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
