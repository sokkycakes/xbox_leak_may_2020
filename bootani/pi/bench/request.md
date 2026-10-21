# Pi bench request 5

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat.xlat | 1cdd7bf3ca8669b0e77b0c5ab1be919c | the translator build with box86's renderer linked in (draws on its own xbc-render thread) and the unaligned-access fix |
| xlat.sh | 580174d36576a6eff073e6741a727da4 | same as request 3 |
| push-xlat.sh | d503e2c227e0aa7df142273ec5604036 | same as request 3 |
| xbprof | be0a3503bc9c5d1f130208244922ec2e | same as request 3 |

(xbcompat.xlat is xbcompat/ at commit 5e9ea141, built in the cloud with
RENDERER=native.)

Thanks for requests 2 to 4. What they showed and what changed since:

- Request 4 never reached the hub and request 3 stalled after it: Phantom
  Dust's unaligned float loads each took a kernel signal (90% of the CPU
  in signal handling). xlat now rewrites each such load once, after its
  first fault.
- xlat's dashboard was slower than box86's (33 vs 38.5 fps) because the
  xlat build drew on the game thread. It now links the same renderer
  box86 uses, so XBCOMPAT_RENDER_WORKER and XBCOMPAT_ASYNC_DRAWS work as
  under box86.
- Request 3 drew the hub's characters as broken grey triangles. Step 3
  below narrows down where that comes from.

## 1. Install

/etc/default/xbox: keep every renderer setting box86 runs with now
(XBCOMPAT_RENDER_WORKER, XBCOMPAT_ASYNC_DRAWS, XBCOMPAT_LATCH_TICKS and the
rest) and add, set the same way, XLAT_PERF_MAP=1 and XLAT_STATS=60. No
BOX86_PROFILE, XLAT_COUNT or XLAT_SAFE_FLAGS. On the PC, from
bootani/pi/bench:

    PI=192.168.1.128 sh push-xlat.sh

If the dashboard doesn't come up, it puts box86 back by itself. Its report
is xlat.txt next to the script. The log should have a line
`D3D: native renderer bridge ABI 4, 312 exports, linked in`, and
`cat /proc/$(pidof xbcompat.arm)/task/*/comm` should list an xbc-render
thread.

## 2. Dashboard

On the main menu, untouched: the `D3D: ... fps` lines over 30 s, then

    /root/xbprof $(pidof xbcompat.arm) 20 1000 40 > /tmp/xbprof-dash.txt

## 3. Phantom Dust hub

Drive to the underground hub (~/Downloads/route-to-hub.sh) and stand still.
Take a screenshot (touch /tmp/xbshot.req) as soon as the hub is up, and
note how long the route took. Wait 30 s, then at the same time

    /root/xbprof $(pidof xbcompat.arm) 30 1000 60 > /tmp/xbprof-hub.txt
    sh /root/clocks.sh 30 > /tmp/clocks-hub.txt

Note the fps lines over that minute and keep the `xlat:` lines from the
last two `xlat: last 60 s:` groups.

If the characters draw right (as on box86), skip to step 4. If they are
still broken, try again with each of these in turn, set the same way in
/etc/default/xbox (one at a time, removing the previous one), restarting
xbox and driving back to the hub each time:

    XLAT_INTERP_OPS=x87
    XLAT_INTERP_OPS=sse
    XLAT_SAFE_FLAGS=1
    XLAT_INTERP_OPS=int

For each try keep a hub screenshot, one fps line and whether the
characters draw right; stop at the first try where they do. The later
tries run much slower (`int` hands nearly all the game's code to the
interpreter, maybe a few fps), so allow the route more time and steer by
screenshots. If Phantom Dust crashes or hangs, keep the last 80 lines of
the log and move on to the next try. Remove the last one when done.

## 4. Hottest code in the hub

Only if the plain run (no XLAT_INTERP_OPS or XLAT_SAFE_FLAGS) reached the
hub. XLAT_COUNT=/tmp/xlathot (mkdir -p it on the Pi), restart xbox, back
to the hub, stand still for 70 s, copy /tmp/xlathot/hot.txt to the PC.
Then remove XLAT_COUNT.

## 5. Where the draws' CPU goes

Add XBCOMPAT_PROFILE_WORK=1 and XBCOMPAT_PROFILE_RENDERER=1, restart xbox,
back to the hub, stand still for 40 s, and keep every log line starting
with `D3D` from the last 30 s. Then remove both.

## 5b. The GPU in the hub

Once the CPU side shrinks, the GPU may be the cap (the fight measured it
busy ~69 ms a frame). Add XBCOMPAT_GPU_COUNTERS=1 and XBCOMPAT_PSH_DUMP=1,
restart xbox, back to the hub, stand still for 40 s. Keep the last two
`D3D GPU counters` groups, one fps line, and every `D3D: pixel shader`
dump with the GLSL lines under it (from the start of the run). Then
replace XBCOMPAT_PSH_DUMP=1 with XBCOMPAT_FLUSH_DRAWS=0, restart, hub,
40 s, and keep the last two counter groups and one fps line again. Then
remove all three.

## 6. Leave and report

    PI=192.168.1.128 sh push-xlat.sh undo

remove XLAT_PERF_MAP, XLAT_STATS, XLAT_COUNT, XLAT_SAFE_FLAGS,
XLAT_INTERP_OPS, XBCOMPAT_PROFILE_WORK, XBCOMPAT_PROFILE_RENDERER,
XBCOMPAT_GPU_COUNTERS, XBCOMPAT_PSH_DUMP and XBCOMPAT_FLUSH_DRAWS from
/etc/default/xbox, restart xbox, and check the dashboard is back on
box86.

Send the results to the cloud session in parts of at most 60 KB, each
starting `request 5 part i/N` (one long message didn't arrive last time):

    request 5: ok / problems, how long the route to the hub took
    xlat.txt (whole), the bridge line, the thread list
    dashboard fps lines, xbprof-dash.txt (whole)
    hub fps lines, xbprof-hub.txt, clocks-hub.txt (whole), the hub's xlat: lines
    for each try in step 3: the setting, fps line, characters right or not
    hot.txt (whole), the D3D lines from step 5
    step 5b: both runs' counter groups and fps lines, the shader dumps
    log tails if anything failed

For each screenshot, say in a line what it shows (characters right or
broken, menu, black screen). If the PC has ImageMagick or ffmpeg, also
send the plain run's hub screenshot as a 320x240 JPEG, base64, in a part
of its own (`convert hub.bmp -resize 320x240 -quality 70 jpg:- | base64 -w0`).

Then keep watching the branch for request 6.
