# Milestone 2026-10-25

Everything the project had built by 2026-10-25, gathered on one branch
(`claude/project-thread-kgb2jx`, tagged `milestone-2026-10-25`). It starts
from the combined Sion build (`claude/project-thread-gudfcx`: games, Guide,
Live/Insignia, Windows game cards), which already contained the earlier
consolidation (`claude/project-thread-p7e5s4`, PR #2), and merges every other
branch on top.

## What is in it

| Area | Where | From branch |
|---|---|---|
| xbcompat runtime, dashboard build, Guide, Live server | `xbcompat/` | gudfcx (includes p7e5s4) |
| Windows game cards, kazeta-guide, FUSE/squashfs/erofs modules | `bootani/sion/`, `xbcompat/src` | project-thread-3ha3ti |
| Guide Now Playing for Windows carts | `xbcompat/src` | project-thread-xujlbp |
| Game card fixes and card order | `bootani/sion/`, `xbcompat/tools` | project-thread-h718yn |
| `sion install-internal` | `bootani/buildroot` | project-thread-9vod8j |
| Frolick on the XAP dash engine, Frolick store, disc streaming (dvd.c, io.c) | `xbcompat/tools/frolickdash`, `xbcompat/src/kernel` | frolick-xap-dash-4fvafa |
| Pi per-draw CPU cuts, bench requests 9 and 10 | `xbcompat/src`, `bootani/pi/bench` | project-thread-ohz82s |
| xbt translator and its Pi kit | `xbt/`, `bootani/pi/xbt-kit` | project-thread-hw53zf, cool-babbage-4ed8f8 |
| frolick-xui (Go XUI scene runtime) | `frolick-xui/` | project-thread-2q18tf |
| LinuxBoot fizz firmware for the Sion | `linuxboot-fizz/` | lucid-thompson-uf283l |
| Qtopia on the RG DS | `qtopia-rgds/` | project-thread-2itiqm |
| Sion Link: DS/3DS Download Play, FFCC client | `sionlink/` | project-thread-rb4kah |
| DS as the Sion's pad (DSpico mirror) | `sionlink/mirror` | project-thread-lkot3d |
| Plans, research and kit notes from the project folder | `docs/project-notes/` | (shared project folder) |

All the older `claude/*` branches were already contained in one of these.

## Merge choices

- `xbcompat/src/kernel/io.c`: volume size reports keep the partition's own
  cluster size (gudfcx) and let Frolick's dashboard see the disk's real size
  (Frolick).
- `xbcompat/src/hle/d3d8.c`: index lists stay compared in full on every draw
  (gudfcx's correctness fix) rather than ohz82s's sampled compare.
- Frolick's XUI runtime keeps the Guide's per-text colour and shadow hooks
  and takes Frolick's progress-bar and ellipsis work.

## Not folded in

- `claude/determined-bardeen-dw96cp` (Pi: same-state draw merging, 16-bit
  DXT uploads, Think Tanks bench request TT1). Its draw path was rewritten
  in parallel with the current one (11 conflicts in `d3d8.c`'s fixed-function
  path) and it was never run on the Pi, so merging it blind could break the
  Sion's rendering. It stays saved on its own branch for a later port.
- Built kits, screenshots and disc images in the shared project folder stay
  there (about 3 GB of binaries); the kits are rebuilt from this source.

## Build check

In the cloud on this branch: `make` in `xbcompat/` (native), `make CPU=xlat`
and `make RENDERER=native` all build. The Frolick XUI runtime passes a
compile check. Not built here: the armhf Pi build and the Wine-hosted
dashboard/Frolick XBEs.
