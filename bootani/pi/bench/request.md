# Pi bench request 5

Written by the cloud Claude session for the Linux PC session, to run when
sokky approves it. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from (branch
claude/determined-bardeen-dw96cp); check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat-native.x86 | fcb5229a9fd5fdaaeb1eb5d4d5aaa633 | the x86 client, xbcompat/ at b9048a43 (`make RENDERER=native`) |
| libxbcompat_renderer.so.1 | d6a853958903a4d757af5b6c36cd7f60 | the ARM renderer for it (`native-renderer.mk TARGET=armhf GL=opengl`) |
| merge.sh | 8d388f01b3f07849c1eba2a969295ecf | Pi side: backs up, installs, rolls back if the dashboard doesn't start |
| push-merge.sh | b65cd189066c9963e557ddb3adb95ad5 | PC side |
| clocks.sh | 65d695544dda3324b349b2480e371bd7 | same as before |

This build merges runs of same-state draws into one GL draw (commit
b9048a43). In an xemu trace of a Think Tanks match, 434 of a frame's 727
draws change nothing but re-set the textures they already had; under
llvmpipe the build sends ~490 of ~850 draws a frame as ~28 GL draws, and
its vertex/index uploads drop from ~12.7 MB to ~0.3 MB a frame.
`XBCOMPAT_MERGE_DRAWS=0` turns it off in the same build, so the two runs
below compare directly.

Requests 2 to 4 are separate (the xlat builds, at commits 651d6364,
6f0550ff and 66b2753a on claude/project-thread-ohz82s). This one doesn't
touch the xlat slot or the CPU governor.

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
the last 30 s (the frame split, `D3D detail:`, `D3D cache:`, `D3D merged:`,
`D3D programs:`). Then the same once more with `XBCOMPAT_MERGE_DRAWS=0`
added. Frame times here don't count.

## 5. GPU counters

Remove the profile and merge lines, add `XBCOMPAT_GPU_COUNTERS=1`, restart,
same match, 30 s, keep the `D3D` GPU counter lines. Then remove it and
`XBCOMPAT_LOG_FPS`.

## 6. Leave and report

If Think Tanks and the dashboard looked right, leave this build installed;
otherwise `PI=192.168.1.128 sh push-merge.sh undo`. Send one message to
the cloud session:

    request 5: ok / problems
    merge.txt (whole)
    fps lines: step 2 (merging) and step 3 (off)
    clocks-tt.txt
    the D3D lines from step 4, both runs
    the GPU counter lines from step 5
    the screenshot, and the log tail if anything failed
