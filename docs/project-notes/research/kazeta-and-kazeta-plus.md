# Kazeta and Kazeta+: how they load games

Researched 2026-10-08 from the two repos' source (cloned at HEAD) and their wikis.

- Kazeta: https://github.com/kazetaos/kazeta (Alesh Slovak, ChimeraOS author; MIT; release 2026-0, last commit 2026-09-30)
- Kazeta+: https://github.com/the-outcaster/kazeta-plus (Linux Gaming Central's fork; MIT; v1.43, last commit 2026-02-28)

## The core idea (both)

A "cartridge" is any removable media (SD card, USB stick, and in Kazeta+ also a CD/DVD) holding a game folder plus a small text file, `*.kzi`. Insert it and power on, and the box boots straight into that game. Boot with nothing inserted and you get the "BIOS", a Rust app for managing saves. Game media stays read-only; saves live on the internal disk.

Kazeta is an Arch-based immutable image (frzr, from ChimeraOS). Each game runs inside **gamescope**, which acts as the compositor directly on KMS/DRM. There's no desktop: the `greetd` autologin runs `kazeta-session`, which runs `/usr/bin/kazeta` (a bash script), and powers off when it returns.

## The three file types

| File | What it is |
|---|---|
| `.kzi` | Plain `key=value` text: `Name`, `Id` (save-slot key), `Exec`, `Icon` (PNG), `Runtime`, `GamescopeOptions` |
| `.kzr` | A **runtime**: an EROFS image (lz4hc, later lzma) holding `.kazeta/share/run` plus whatever runs the game |
| `.kzp` | A **packaged cart**: the whole game folder as one compressed EROFS image, loop-mounted at launch |

Example `.kzi` for an emulated game:
```
Name=Sonic the Hedgehog
Id=sonic-1
Exec="Sonic The Hedgehog (USA, Europe).md"
Icon=icon.png
GamescopeOptions=--filter pixel
Runtime=megadrive
```

## How a launch works (`rootfs/usr/bin/kazeta`)

1. **Find the cart.** `udev-media-automount` mounts removable media under `/media` or `/run/media`. The script polls `find <media> -maxdepth 2 -name '*.kzi' / '*.kzp'` for 2 s (10 s if `/dev/sr*` exists). It finds nothing, so it runs `gamescope -- kazeta-bios` instead.
2. **If it's a `.kzp`,** it mounts the image and finds the `.kzi` inside.
3. **Resolve the runtime.** `Runtime=windows` matches `windows`, then `windows.kzr`, then `windows-*.kzr`, all **on the cart media**. If none matches, it falls back to `/usr/share/kazeta/runtimes/none.kzr`.
4. **Build the game's filesystem** with `kazeta-mount`, run as root through polkit:
   - loop-mount the `.kzr` runtime
   - `mount -t overlay -o metacopy=on,xino=off,lowerdir=<cart>:<runtime>,upperdir=<saves>/<Id>,workdir=...`
   So the game sees one merged directory made of cart files, runtime files and its own writable save layer. Every write (configs, saves, Proton prefixes) lands in `~/.local/share/kazeta/saves/default/<Id>`. That's what makes per-game saves automatic, with no help from the game.
5. **Run it.** `HOME` and every `XDG_*` variable point at the merged dir. `Exec` is written to `/tmp/kazeta-cart-exec`, then `gamescope <opts> -- ./.kazeta/share/run /tmp/kazeta-cart-exec`. The **runtime's `run` script decides how `Exec` is interpreted.**
6. Playtime is logged every 60 s. On exit, a trap unmounts everything.

So the "different kinds of games" are handled entirely by **which runtime image gets overlaid**:

| Kind | Runtime | What `run` does |
|---|---|---|
| Native Linux | `linux` (umu + Steam Runtime "sniper" container) | runs `Exec` inside pressure-vessel |
| Windows | `windows` (umu + sniper + GE-Proton) | runs `Exec` through Proton |
| Emulated | `megadrive`, `snes`, `nes`, `n64` (upstream); `dolphin`, `dreamcast`, plus unofficial PS1/Saturn/Sega CD/PC Engine (Kazeta+) | a RetroArch AppImage plus one libretro core `.so`; `Exec` is the ROM path |
| Static binary | `none` (empty) | runs `Exec` as-is |

The OS itself knows nothing about Windows or consoles. Adding a system means shipping a new `.kzr`.

## What Kazeta+ changes

- **Runtimes on the internal disk.** The lookup order becomes `/usr/share/kazeta/runtimes` first, then the cart media, then inside the `.kzp`, then `none`. The BIOS has a Runtime Downloader (`kazeta-runtime-helper install/delete`). Carts no longer need to carry a 1+ GB Proton runtime.
- **Multi-cart.** Several `.kzi`/`.kzp` can sit at the media root. Autoboot picks the one with `SetAsDefaultGame=true`, or else the first one found. The BIOS has a Game Selection screen. Picking a game writes `/usr/bin/kazeta '<path.kzi>'` to `/var/kazeta/state/.LAUNCH_CMD` and restarts the session. `kazeta-session` evals that command, then `exec`s itself, which drops back to the BIOS when the game quits. An `autoboot=false` option always boots to the BIOS.
- **Optical discs.** A udev rule on `sr*` starts `optical-mount@` to mount the disc at `/run/media/srN`. The disc is just an ISO9660+Joliet+RR image of a cart folder (`genisoimage -J -R`). It's labelled experimental, and Linux/Windows runtimes take minutes to load off a disc. There's also a CD audio player in the BIOS.
- **Per-cart controller profile.** A `Controller=` key loads an InputPlumber profile over D-Bus before launch.
- **Launch-failure screen.** If `Exec` exits non-zero within 10 s, the tail of `run.log` goes into `session.log` and an error screen shows it.
- **Other:** 4:3 and other resolution/aspect options, Bluetooth, GameCube adapter, themes, OTA updates, a log-copy button. It switched from greetd to lightdm with a Wayland session entry, and the image now ships xorg-server too.
- It **dropped `xino=off`** from the overlay mount, which upstream added because "32-bit applications crash" without it.
- Kazeta+ runtimes are reported to be backward-compatible with stock Kazeta.

## Relevance to Sion

What maps across cleanly:

1. **Runtime as an overlaid image.** This fits xbcompat well. Add an "xbox" runtime (xbcompat plus its i686 SDL2 and Mesa) and treat a game as a folder with `default.xbe` and a small info file. Frolick's libretro cores are the same idea as Kazeta's emulator runtimes: one RetroArch (or libretro frontend) plus a core per system, with the ROM as `Exec`. The real difference is the frontend. Ours would be Theseus or Frolick rather than a save-manager BIOS.
2. **Saves via overlayfs upperdir keyed by game Id.** This answers xbcompat's open "map C:/E: to host dirs" item without per-title code. Lower is the read-only USB stick, upper is `/var/.../saves/<TitleID>`. The XBE title ID is a natural `Id`.
3. **The launch loop.** Kazeta+'s `.LAUNCH_CMD` plus session-restart pattern is the supervisor that `dashboard-on-sion.md` blocker 3 asks for: the dashboard calls `XLaunchNewImage`, xbcompat writes a launch file and exits, the session script runs the title, and when the title exits the dashboard comes back. Their version is about 20 lines of shell.
4. **`xino=off`.** xbcompat is a 32-bit process. If titles run from an overlayfs, keep `xino=off`, which is upstream's fix for 32-bit apps getting EOVERFLOW from `stat` on large inode numbers.
5. **Detection.** Polling `find -maxdepth 2` for 2 s after automount is crude but works. Sion already mounts the USB stick only on demand, so the same check fits into theseus-session.

What doesn't fit:

- **gamescope, Vulkan, PipeWire, polkit, InputPlumber, Steam Runtime.** All of it is heavy for a Buildroot image, and gamescope needs Vulkan. On Sion, SDL2 KMSDRM, with `KMS_MODE=720x480` and Theseus's fixed 640x480 scene, already does gamescope's scaling job, so there's nothing to gain by adopting it.
- **Proton and Windows games.** These need the full Steam Runtime (glibc, x86_64 plus lib32). Out of scope for Sion.
- **Kernel config** if we copy the model: `CONFIG_EROFS_FS` (with LZ4/LZMA), `CONFIG_OVERLAY_FS` and `CONFIG_BLK_DEV_LOOP`. Plain directories on the stick work without EROFS. Overlayfs is the only part that really matters.
