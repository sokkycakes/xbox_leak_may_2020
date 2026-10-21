# x86 / Raspberry Pi runtime synchronization

The Pi uses the same D3D, kernel, input and audio sources as the x86 build.
The i386 runtime executes the XBE through Box86; only the renderer runs as
native ARM code. Rebuilding the renderer alone does not update the runtime,
launchers, Dashboard scripts or display keeper.

## Synchronization scope

The synchronization starting from upstream dc22da5 includes its vblank and
miniport event handling, soft reset, shader/render-state and title fixes,
DVD handling, and Dashboard screensaver changes. It retains the native
renderer, authored mipmaps, Dashboard audio compensation, removable-card
refresh, and automatic per-XBE HLE mapping from the Pi work.

Build and deploy these together:

- i386 runtime with RENDERER=native and its matching ARM renderer library.
- Box86 with the Pi fixes and native renderer wrapper.
- Native xbox-av display keeper and the shared Xbox session/launch scripts.
- xbrun/xbsymmap tools, native symbol scanner and XDK signature caches.
- Rebuilt Dashboard XBE, matching HLE map, and signed XIP assets.

The March 2001 Games pod assets must survive a Dashboard rebuild. Running
patch_dash.py without the NewGames source assets selects a different Games
layout; that is not a valid parity update for this installation.

## Display handoff

The Dashboard can persist its loading frame, but a separate xbox-av process
must retain DRM ownership between executables. The old Pi installation had
no keeper binary. Its new ARM build also needs 64-bit file offsets: DRM
buffer offsets exceed the 32-bit mmap offset range on this hardware.

A real Pi 3 KMS check sent a 64x48 test image to the keeper, then issued the
next-title notification. The active 720x480 framebuffer ID and all pixel
bytes stayed unchanged (FNV-1a 9cf229c5). This checks scanout persistence,
not just a successful socket reply. Actual Dashboard/title launch results
are recorded below after deployment.

## Persistent Pi layout

- `/opt/xbcompat/bin/xbcompat-native.x86`: synchronized Box86 client.
- `/opt/xbcompat/native/lib/libxbcompat_renderer.so.1`: matching ARM backend.
- `/usr/bin/box86`: Pi fixes and native renderer wrapper.
- `/opt/xbcompat/bin/xbox-av`: native display keeper.
- `/opt/xbcompat/bin/xbcompat.x86` and `xbcompat.arm`: synchronized builtin
  and Unicorn fallbacks. `XBOX_RENDERER=builtin` or `XBOX_CPU=unicorn` in
  `/etc/default/xbox` select them. Native rendering is the normal Pi default.

Buildroot now produces and installs all these components. The native library
uses `native/lib`, avoiding the Sion-specific Mesa paths selected by an
`/opt/xbcompat/lib` directory. Sion bundle builds now run the incremental
Dashboard build even when an old XBE exists. dashbuild can preserve existing
March 2001 archives through `NEWGAMES_XIPS`, and keeps them automatically
on subsequent incremental rebuilds.

The live Pi retains 4x MSAA, 1024-frame headphone audio and the approved
12 dB Dashboard-only boost. A SHA-256 deployment manifest is installed as
`/opt/xbcompat/SOURCE-SYNC.sha256`. Runtime/script and Dashboard backups are
`source-sync-backup.tar` and `dashboard-before-source-sync.tar` in that directory;
the prior Unicorn binary is `bin/xbcompat.arm.before-source-sync`.

## Validation

- Built builtin i386, native-client i386, ARM renderer, i386 test renderer,
  ARM display keeper and ARM Unicorn 2.1.4 runtime.
- 60,000 entry ABI calls passed. ABI 2 validates all 311 D3D export names/order.
- Rendered triangle and authored-mipmap pixel checks passed on both Mesa
  llvmpipe and the actual Pi VC4. Vblank callbacks, miniport event signaling,
  reentrant callbacks and reset host services passed through Box86/ARM.
- DirectSound: 255 checks; XInput: 167 checks; card/mapping/launcher Python
  suites: 27 tests. No failures.
- Rebuilt and signed Dashboard XBE and regenerated its map. All 22 XIP
  hashes were compared: only default.xip changed; the Games assets match.
- The actual XBLA chain ran from the disc menu to content/default.xbe, then
  to installed Ms. Pac-Man; a 480-frame run exited successfully and its
  screenshot shows the game playfield. The keeper held a 720x480 frame for
  about 20 seconds during the first child map preparation, then preserved
  another frame at the game handoff. Directly invoking the game without
  Arcade launch data returned to the Dashboard; the normal chain supplies it.
- Dolphin rendered successfully, and a separate L+R+Back+Start test logged
  soft reset and returned successfully to the Dashboard supervisor.
