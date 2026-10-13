# Pi bench request 1

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval ("Pi: run the speed tests on your own"). Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| libxbcompat_renderer.so.1 | 60c8502004c2ce069734351607d85d27 | push-async5 renderer + screenshots on request |
| xbcompat-native.x86 | 3f780746d9e25ecee49955511aed4dc4 | x86 runtime + pad pipe |
| xbprof | e0453100f2b93e91e2f354d450ee37c0 | whole-process profiler (static armhf) |
| push-vblank.sh | dd25777b766a67d5d9da536667a3795d | unchanged |
| vblank.sh | 28cea926a04fcafd0b3b0d305dc863e0 | unchanged |
| S02cpufreq | c385fd2c08ee04ef2c1b75b8f369b8d0 | unchanged |
| clocks.sh | 65d695544dda3324b349b2480e371bd7 | unchanged |

## 1. Install

Run `sh push-vblank.sh` from the download folder. Copy xbprof to
/root/xbprof on the Pi (`chmod +x`).

In /etc/default/xbox keep XBCOMPAT_RENDER_WORKER=1, XBCOMPAT_ASYNC_DRAWS=1,
the latch and work-profile settings, and add, set the same way as those:

    XBCOMPAT_PAD_PIPE=/tmp/xbpad
    XBCOMPAT_SHOT_ON_REQUEST=/tmp/xbshot

Restart xbox.

## 2. Find the way to the hub

- Press a button: `timeout 5 sh -c 'echo a > /tmp/xbpad'`. Without the
  timeout, a write blocks while no xbcompat process has the pipe open
  (between the dashboard and the game). Words run in order: `"down down
  a/30"`. `/N` holds for N 60ths of a second; `wait/120` pauses 2 s; `lt+rt`
  presses both. The buttons: a b x y black white lt rt start back up down
  left right lthumb rthumb, and lsup/lsdown/lsleft/lsright (rs... too).
- See the screen: `touch /tmp/xbshot.req`, wait until it disappears, then
  fetch /tmp/xbshot.bmp (720x480, 32-bit BMP).
- Launch Phantom Dust however is quickest (the dashboard's Games area, or
  the run-phantom.sh you used before, pointed at E:\Games\Phantom Dust),
  load the save and walk to the underground hub, the spot of the earlier
  hub measurements. Keep the route as a script (on the PC is fine) with
  generous waits, checked by screenshots, so later requests just run it.

## 3. Measure (all in the hub, standing still)

A. Baseline: 60 s with the settings above. Report the median ms per frame
   and fps, the "queued drawing" line (its state-copying figure), the
   "presenting thread" line and any "worker blocked" lines over 1 ms.
   Compare with push-async4 there (119.6-132 ms a frame). If push-async5
   crashes or draws wrong, run `sh /root/vblank.sh undo` and report that.

B. Profiles: add `XBCOMPAT_PROFILE=/tmp/xbguest.txt` to /etc/default/xbox,
   restart, go back to the hub, wait 30 s, then run
   `/root/xbprof $(pidof xbcompat-native.x86) 30 1000 40` on the Pi.
   Wait another 30 s and copy /tmp/xbguest.txt. Send xbprof's whole output
   and the first 200 lines of xbguest.txt. Remove XBCOMPAT_PROFILE after.

C. box86 settings: one run each, restart and back to the hub, 45 s of fps,
   median ms per frame. Set each variable the same way as the others:
   1. BOX86_DYNAREC_BIGBLOCK=2
   2. BOX86_DYNAREC_CALLRET=1
   3. BOX86_DYNAREC_SAFEFLAGS=0
   4. BOX86_DYNAREC_FORWARD=1024
   5. all of 1-4 that were faster than A by 2% or more, together
   If a variant crashes or draws wrong, note it and move on. Check the
   process environment (`tr '\0' '\n' < /proc/$(pidof xbcompat-native.x86)/environ | grep BOX86`)
   once to confirm the variables reach it.

## 4. Leave and report

Leave /etc/default/xbox as in step 1 (no BOX86_ variants, no profiler),
restart xbox, and send one message to the cloud session with:

    request 1: installed ok / rolled back (why)
    route to the hub: works / problems
    A: ms/frame, fps, state copying, presenting thread, worker blocked
    B: xbprof output, xbguest.txt first 200 lines
    C: each variant's ms/frame (and crashes)
    anything odd (temperatures over 80 C, throttling, errors in the log)
