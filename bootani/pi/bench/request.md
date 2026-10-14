# Pi bench request 7

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat.xlat | 88864fa5c5446fb6236728c8af8719bc | the translator build with the fixes below |
| xbprof | 14e38d6997c96a87bbdb7ae80699d44a | same as request 6 |
| xlat.sh | 580174d36576a6eff073e6741a727da4 | same as request 3 |
| push-xlat.sh | d503e2c227e0aa7df142273ec5604036 | same as request 3 |

(xbcompat.xlat is xbcompat/ at commit 1cb7b97c, built in the cloud with
RENDERER=native.)

Thanks for request 6. What it showed and what changed since:

- The characters broke because Phantom Dust has a function that returns
  its answer in a CPU flag, and xlat dropped flags at returns. xlat now
  keeps those flags at every call, return and indirect jump, for every
  game; no setting is needed any more.
- The hub's SSE math now stays in ARM registers within a block (Phantom
  Dust's hottest block went from 385 to 118 ARM instructions).
- Inaudible sound voices still cost most of a core because they were
  advanced one sample at a time; they now advance 32 samples or a whole
  block at a time, so the mixer holds its lock for much less.
- The game thread compared state with glibc's memcmp dozens of times a
  draw, and the render worker copied 3 KB of shader constants per draw;
  both are cheaper now.
- The fps still sank from 11 to ~8.5 over three minutes. This request
  checks whether box86 sinks the same way, and profiles the start and
  the end of the sink separately to see what grows.

## 1. box86 first

The Pi is on box86 now; keep it that way for this step. Restart xbox,
drive to the underground hub (~/Downloads/route-to-hub.sh) and stand
still. From the moment the hub is up, keep every `D3D: ... fps` line and
every `DSound:` line for three minutes.

## 2. Install

/etc/default/xbox: keep every renderer setting box86 runs with now
(XBCOMPAT_RENDER_WORKER, XBCOMPAT_ASYNC_DRAWS, XBCOMPAT_LATCH_TICKS,
XBCOMPAT_LOG_FPS and the rest) and add, set the same way, XLAT_PERF_MAP=1
and XLAT_STATS=60. No XLAT_SAFE_FLAGS, XLAT_FLAG_CHECK or
XBCOMPAT_MIX_SILENT. On the PC, from bootani/pi/bench:

    PI=192.168.1.128 sh push-xlat.sh

If the dashboard doesn't come up, it puts box86 back by itself. Its report
is xlat.txt next to the script. The log should have the line
`D3D: native renderer bridge ABI 4, 312 exports, linked in`.

## 3. xlat hub, three minutes, profiled twice

Restart xbox, drive to the hub, stand still, and screenshot (touch
/tmp/xbshot.req) as soon as it's up: the player and the NPC should now
draw right with no flag setting. From that moment keep every fps and
`DSound:` line for three minutes, and every `xlat:` line. Profile twice,
each time with both commands at once:

at 30 s:

    /root/xbprof $(pidof xbcompat.arm) 30 1000 60 > /tmp/xbprof-early.txt
    sh /root/clocks.sh 30 > /tmp/clocks-early.txt

at 150 s:

    /root/xbprof $(pidof xbcompat.arm) 30 1000 60 > /tmp/xbprof-late.txt
    sh /root/clocks.sh 30 > /tmp/clocks-late.txt

If Phantom Dust crashes or hangs, keep the last 80 lines of the log.

## 4. Leave and report

    PI=192.168.1.128 sh push-xlat.sh undo

remove XLAT_PERF_MAP and XLAT_STATS from /etc/default/xbox, restart xbox,
and check the dashboard is back on box86.

Send the results to the cloud session in parts of at most 60 KB, each
starting `request 7 part i/N`:

    request 7: ok / problems, how long the route to the hub took
    step 1: box86 fps and DSound lines
    xlat.txt (whole)
    step 3: characters right or not, fps and DSound lines, the xlat: lines
    step 3: xbprof-early.txt and xbprof-late.txt (whole, apart from the
            dpc and SDLHotplugALSA threads' detail), clocks-early.txt and
            clocks-late.txt
    log tails if anything failed

Say in a line what the hub screenshot shows, and send it as a 160x120
JPEG, base64, in a part of its own, as in request 6 part 4.

Then keep watching the branch for request 8.
