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

2026-10-25 fix 2: on KMSDRM the second window xemu's offscreen GL layer opens
can't exist ("glo_context_create: Failed to create window"), so its contexts
are now made against the display window and run surfaceless.
XBCOMPAT_GPU_SURFACELESS=1 forces that path elsewhere (tested in the cloud
with SDL_VIDEO_X11_FORCE_EGL=1: tut02 and Doom 3 render).

2026-10-25 fix 3: SDL's KMSDRM console keyboard installs its own SIGSEGV
handler at SDL_Init, which broke the NV2A MMIO trap (the game "crashed" on
its first GPU register access). gpu_display_init now re-installs xbcompat's
fault handlers after creating the window, as the normal build does.

2026-10-25 fix 4: on the Sion the display window's surface came back
EGL_BAD_MATCH (every swap failed, black screen): SDL re-picks the EGL config
when it rebuilds the KMSDRM surface at the first swap, and by then
gloffscreen had changed the GL attributes (8888, depth 24, stencil 8). The
display window now asks for the same attributes up front.
