# xbcompat-gpu for games: Sion kit

- xbcompat: the Sion's live build (xujlbp de9d94a9: cards, Live, Guide pages,
  streaming, MechAssault fixes) plus one switch: when /data/xbox/use-gpu
  exists, games (not the stock dashboard, not Frolick) re-exec xbcompat-gpu
  from the same folder.
- xbcompat-gpu: the same tree built with GPU=1 (xemu's NV2A emulated; the
  title's own Direct3D runs). No Guide overlay in it.
- lib/: libraries xbcompat-gpu needs that /opt/xbcompat/lib lacks (only
  copied when missing).

Install: keep /opt/xbcompat/bin/xbcompat as xbcompat.pre-gpu-<date>, copy
xbcompat and xbcompat-gpu into /opt/xbcompat/bin (and /data/overlay/opt/...),
copy missing lib/ files, touch /data/xbox/use-gpu, restart the dashboard.
Off: rm /data/xbox/use-gpu. Undo fully: put xbcompat.pre-gpu back.

2026-10-25 fix: xbcompat-gpu now asks xbox-av for the display (av_title_starting)
before SDL_Init, like the normal build's CreateDevice. Without it every game
failed with "SDL_Init: kmsdrm not available". Only xbcompat-gpu changed; swap it
in and the next game launch uses it.
