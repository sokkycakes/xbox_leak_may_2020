# push-guidehud

The Sion's combined xbcompat (push-compat27's build) with the Guide drawn into
a HUD buffer of its own, as the console's XAM does: 640x480 (852x480 when the
dashboard's widescreen setting is on), then laid over the game in one stretch.
The Guide's size and place no longer depend on the game's resolution or the
output's; on the Sion's 480-line output it is drawn at 480 lines and
stretched sideways only.

Source: branch claude/project-thread-p3ns35 = claude/project-thread-di7q3m
14c07205 merged with claude/project-thread-xujlbp f4ae70a1 (io.c conflict
resolved: both the DVD volume branch and Frolick's cache reservation) plus
the HUD buffer commit, and the GPU-build switch
(/mnt/project-files/xbcompat/sion-compat27/gpu-switch-on-merge.patch, kept
outside the branch). Built with `make GL=opengl` against i386 SDL2,
interpreter set to /opt/xbcompat/lib/ld-linux.so.2, stripped. Without the
HUD buffer change this tree builds push-compat27's xbcompat (3410bbbd) byte
for byte except the build id.

`kazeta-guide` (Windows game cards' Guide) is rebuilt from the same
guide.cpp with push-wincart/kazeta-guide/build.sh, so Windows cards get the
same HUD buffer. Its old alpha patch is folded into guide.cpp.

`guide.tar.gz`, `launch-title` and `xbsymmap.py` are push-compat27's.

Install with `install.sh` from the PC (refuses while a game, xbcompat-gpu or
a Windows card is running). XBCOMPAT_GUIDE_HUD=4:3|16:9|off in
/etc/default/xbox forces the buffer's shape or turns it off.
