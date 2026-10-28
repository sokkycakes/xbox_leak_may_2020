# push-compat27

The Sion's combined xbcompat with the 2026-10-26/27 compatibility passes.

Source of `xbcompat`: claude/project-thread-xujlbp f4ae70a1 (Guide, Live,
Windows game cards, streamed discs) merged with
claude/project-thread-di7q3m (aa97be22, then the Hulk intro fix and the Halo 2 second-CreateDevice window fix 01fff117, the Halo 2 visibility tests e4ed3a3e and the shared depth for texture targets 306ca02d + its fix), plus the GPU-build switch from
claude/project-thread-8q0kad (gpu-switch-on-xujlbp.patch, its conflicts
resolved; kept outside this branch). Built with `make GL=opengl` against
i386 SDL2, interpreter set to /opt/xbcompat/lib/ld-linux.so.2, stripped.

`guide.tar.gz`, `kazeta-guide` and `launch-title` are xujlbp f4ae70a1's,
the ones on the Sion now. Install with `install.sh` from the PC.

`xbsymmap.py` is this branch's xbcompat/tools/xbsymmap.py. The game maps
are made on the Sion with /opt/xbcompat/tools, so without it the signatures
these passes added (Halo 2's vertex shader calls among them) never reach the
map and their fixes don't apply: Halo 2 then shows no picture. The old one
is kept as xbsymmap.py.pre-compat27.
