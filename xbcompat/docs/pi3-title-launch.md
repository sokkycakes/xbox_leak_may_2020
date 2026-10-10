# Pi title handoff and precomputed HLE maps

The ARM Pi image does not include the Python/xbrun signature scanner. Before
this fix, Dashboard `XLaunchNewImage` discarded the Dashboard HLE map and
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
A missing or zero-byte map returns exit status 78 without running the guest.
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

The destination directory is created by `xbox-session`. Map generation runs on
the host, not the Pi. `tools/pi/titles.sh` also packages checksum maps for its
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

Nine host regression tests cover argument forwarding, paths with spaces,
checksum isolation, sidecar selection, absent/empty maps, launcher overrides,
and preparing a checksum map. Run them on Linux with:

```sh
python3 xbcompat/tests/title_launcher_test.py
```
