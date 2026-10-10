# Pi title handoff and automatic HLE maps

The original ARM Pi image omitted the Python/xbrun signature scanner. Before
these fixes, Dashboard `XLaunchNewImage` discarded the Dashboard HLE map and
re-executed xbcompat without a map for the selected game. Dolphin reached raw
Xbox D3D hardware code, faulted at `0xfd001804`, and exited. A standalone test
launch had no session supervisor, so this left the console visible.

`xbox-env` now selects `/usr/libexec/xbox-title` when the bundled Python launcher
is unavailable. The shell launcher looks for a map in this order:

1. `$XBCOMPAT_MAP_DIR/<full-file-SHA1>.map`, or, by default,
   `$XDG_CACHE_HOME/xbcompat/maps/<full-file-SHA1>.map` (normally
   `/data/xbox/cache/xbcompat/maps/`).
2. `<absolute-XBE-path>.map`, an explicitly paired sidecar.
3. `xbcompat.map` beside an image named exactly `default.xbe`, matching the
   bundled sample layout. Keep this sidecar paired with its original executable.

Checksums distinguish samples that share `default.xbe` and title ID `FFFF0000`.
If no prepared map is available, the launcher now runs native Python and the
ARM XbSymbolDatabase scanner through `xbrun.py --map-only`. The game runs only
after successful map generation. Scanner errors return exit status 78.
The existing `xbox-session` supervisor then starts the Dashboard again. This
requires launching the session through `xbox-session`; directly running
xbcompat has no automatic recovery. Systems with bundled Python retain their
existing on-device scanner. Explicit runtime and launcher overrides are kept.

## Preparing another card title

On a host with xbcompat's normal map-generation dependencies:

```sh
python3 xbcompat/tools/pi/cache-map.py /path/to/default.xbe --cache ./title-maps
```

Or use an already generated map for that exact executable:

```sh
python3 xbcompat/tools/pi/cache-map.py /path/to/default.xbe \
  --map /path/to/xbcompat.map --cache ./title-maps
scp ./title-maps/*.map root@PI:/data/xbox/cache/xbcompat/maps/
```

The destination directory is created by `xbox-session`. This host command remains available for preparing cards in advance.
Ordinary card launches now generate missing maps directly on the Pi. `tools/pi/titles.sh` also packages checksum maps for its
bundled samples, and the session seeds them into the writable cache. A map
selects HLE replacements; it does not guarantee that every function a new game
uses is implemented.

## Pi 3 validation

Tested the card's debug XDK 4400 Dolphin executable (SHA-1
`e717c04d07e3966d2b6cfed03aa1da321e804a72`). Its map was generated from the
matching D3D8D/XAPILIBD libraries. The runtime loaded 1,816 symbols, replaced
118 functions, and initialized native renderer bridge ABI 1. A bounded
180-frame run exited successfully; a frame-120 screenshot showed the rendered
dolphin scene. Native ARM rendering used 4x MSAA at 640x480 on the 720x480
display mode.

The subsequent live Dashboard-to-card handoff selected the checksum map,
retained the native runtime and GameCard D: path, and sustained 59.9 fps.
The game remained a child of `xbox-session`; it was left running for the user.
The existing restart loop handles title exits; forced-crash recovery was not
separately exercised during this validation. Dashboard audio settings were
retained.

Seventeen host regression tests cover argument forwarding, paths with spaces,
checksum isolation, sidecars, automatic scanning, failed/empty scans, kernel-only
images, runtime identity, cache invalidation, and atomic publication. Run them on
Linux with:

```sh
python3 xbcompat/tests/title_launcher_test.py
python3 xbcompat/tests/map_cache_test.py
```

## Automatic scanning and Xbox Live Arcade

The initial shell launcher fixed Dolphin by supplying a precomputed map, but
Arcade still lacked one and returned status 78 before guest execution. The ARM
image now packages native Python, the same pinned XbSymbolDatabase revision as
the x86 bundle, the shared mapping scripts, and all bundled XDK 4400 signature
sets. First launches can take tens of seconds to scan; subsequent launches use
the generated cache. No download or compiler is needed on the Pi.

The shell launcher runs the mapper as a subprocess, keeping its Python
environment separate from the game. `XBCOMPAT_HLE_BINARY` names the actual
runtime ELF; `XBCOMPAT_BIN` may name a shell wrapper invoking Box86. The normal
Pi wrapper exports the selected ELF before running the Dashboard or a game.
Custom wrappers must do the same. Generated maps are keyed by the complete XBE
checksum plus runtime/scanner/adapter identity. XDK 4400 maps additionally
track their signature data and scanner script. Only complete, nonempty maps
are published atomically. The database scanner has a 120-second timeout.

On the live Pi, native scanning generated 244 mappings for the Arcade disc
menu. A 240-frame startup test rendered that menu with 4x MSAA and exited
successfully. The subsequent user-driven session launched `content/default.xbe`
(the Arcade application), returned to the disc menu, launched `CDXU/reboot.xbe`,
and returned normally to the Dashboard. The reboot helper also validated the
XDK 4400 signature path. Other disc executables are handled individually by the
same automatic scanner; map generation alone does not establish gameplay
compatibility for each one.

The live deployment uses native Debian armhf Python 3.11 in the isolated
`/opt/xbcompat/map-python` directory and a cross-compiled native scanner. Future
images use Buildroot's native Python and scanner package. The scanner was
cross-built and executed on the Pi; a complete replacement image was not built
or reboot-tested in this validation. Existing native-renderer and Dashboard
audio settings were retained.
