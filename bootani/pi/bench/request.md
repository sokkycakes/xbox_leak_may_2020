# Pi bench request TT1 (Think Tanks)

Not part of the Phantom Dust requests. Those come from another cloud
session on branch claude/project-thread-ohz82s, which numbers its own
(it is at request 9); this branch's request was first called request 5
too, hence the new name. Revised at dba23f0a: the renderer changed (md5
below). If the earlier version already ran, run this one again from
step 1.

Run it only when none of the Phantom Dust requests is in progress and the
Pi is back on box86 (their `push-xlat.sh undo`): step 1 restarts xbox,
which would end a run of theirs, and on xlat (XBOX_CPU=unicorn) this
build wouldn't run at all. merge.sh refuses to install unless
/etc/default/xbox selects box86 with the native renderer. Once installed,
this is the box86 build every title runs, Phantom Dust too, until
`push-merge.sh undo`.

Written by the cloud Claude session for the Linux PC session, to run when
sokky approves it. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from (branch
claude/determined-bardeen-dw96cp); check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat-native.x86 | fcb5229a9fd5fdaaeb1eb5d4d5aaa633 | the x86 client (`make RENDERER=native`); unchanged since d119671c, as everything since is in the renderer |
| libxbcompat_renderer.so.1 | dd87db240bf0254ac83c6210bc914650 | the ARM renderer, xbcompat/ at dba23f0a (`native-renderer.mk TARGET=armhf GL=opengl`) |
| merge.sh | b3800efcc2e7b4015f87fc71063ce5cd | Pi side: refuses unless the Pi is on box86; backs up, installs, rolls back if the dashboard doesn't start |
| push-merge.sh | 960d702ba7955e6020a559c5780a37e7 | PC side |
| clocks.sh | 65d695544dda3324b349b2480e371bd7 | same as before |

This build has two changes, each with a switch in the same build, so the
runs below compare directly:

- It merges runs of same-state draws into one GL draw (b9048a43 for
  shader draws, dba23f0a for fixed-function fans, strips and lists). In an
  xemu trace of a Think Tanks match, 434 of a frame's 727 draws change
  nothing but re-set the textures they already had, and ~85 more are
  fixed-function quads with identical state. Under llvmpipe the build
  sends 115-195 GL draws a frame in a match where it sent 660-865
  before, and its vertex/index uploads drop from ~12.7 MB to ~0.3 MB a
  frame. `XBCOMPAT_MERGE_DRAWS=0` turns both off.
- DXT textures go to the GPU as 16-bit texels when they are opaque or have
  only on/off alpha (d119671c), instead of Mesa's 32-bit expansion: half
  the texture traffic for 54 of Think Tanks' 95 DXT textures, at most
  3/255 off per channel. `XBCOMPAT_DXT16=0` turns it off;
  `XBCOMPAT_DXT16=2` also sends partial-alpha textures as RGBA4444, which
  costs some image quality.

One more change (ff7cdabd) can raise the step 2 and 3 frame rates over
earlier requests' without changing the rendering: `XBCOMPAT_LOG_FPS=1` on
its own prints only the rate, but it used to time every draw too, and here
in the cloud those clock reads took 41% of the render thread. They now run
only with `XBCOMPAT_PROFILE_WORK=1` (step 4), so compare fps only within
this request, not with earlier ones.

This request doesn't touch the xlat slot (xbcompat.arm) or the CPU
governor.

## 1. Install

On the PC, from bootani/pi/bench:

    PI=192.168.1.128 sh push-merge.sh

Its report is merge.txt next to the script. If the dashboard doesn't come
up, the earlier build goes back by itself; then stop and report.

## 2. Think Tanks with merging (the default)

In /etc/default/xbox set `XBCOMPAT_LOG_FPS=1` (and nothing else from the
earlier requests), restart xbox, start Think Tanks from the Arcade card the
usual way, then Single Player → A on the tank → QuickPlay (Battlemode,
Medulla Mesa). Once in the match, press Y if the brain is out, stand still
looking across the map for 10 s, then keep the fps lines from the next 60 s
and run, at the same time:

    sh /root/clocks.sh 30 > /tmp/clocks-tt.txt

Take a screenshot (touch /tmp/xbshot.req) and look at it: terrain, rocks,
tanks and the HUD should look as they did before this build.

## 3. Same, with merging off

Add `XBCOMPAT_MERGE_DRAWS=0`, restart xbox, same match and spot, 60 s of
fps lines. Then remove that line.

## 4. Where the frame goes

Add `XBCOMPAT_PROFILE_WORK=1` and `XBCOMPAT_PROFILE_RENDERER=1`, restart,
same match, stand still 40 s, and keep every line starting with `D3D` from
the last 30 s (the frame split, `D3D detail:`, `D3D cache:`, `D3D merged:`
(its last number counts every GL draw), `D3D programs:`). Then the same once more with `XBCOMPAT_MERGE_DRAWS=0`
added. Frame times here don't count.

## 5. 16-bit DXT textures

Remove the profile and merge lines. Three runs, each a restart, the same
match and spot, 60 s of fps lines and the `D3D` GPU counter lines, with
`XBCOMPAT_GPU_COUNTERS=1` set in all three:

- as installed (opaque and on/off-alpha DXT as 16-bit),
- with `XBCOMPAT_DXT16=0` (all DXT 32-bit, as before),
- with `XBCOMPAT_DXT16=2` (partial alpha as RGBA4444 too); take a
  screenshot here and look for banding on smoke, sky and the HUD.

Then remove `XBCOMPAT_GPU_COUNTERS`, `XBCOMPAT_DXT16` and
`XBCOMPAT_LOG_FPS`.

## 6. Leave and report

If Think Tanks and the dashboard looked right, leave this build installed;
otherwise `PI=192.168.1.128 sh push-merge.sh undo`. Send one message to
the cloud session:

    request TT1: ok / problems
    merge.txt (whole)
    fps lines: step 2 (merging) and step 3 (off)
    clocks-tt.txt
    the D3D lines from step 4, both runs
    step 5: fps and GPU counter lines for each of the three runs
    the screenshots (step 2 and step 5's DXT16=2), and the log tail if
    anything failed
