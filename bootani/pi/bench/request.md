# Pi bench request 6

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat.xlat | 6c6d2ad54fd75151ef7c7f9bb9b4925f | the translator build, with the flag check below, x87 sin/cos/atan/exp translated, and inaudible sound voices skipped |
| xbprof | 14e38d6997c96a87bbdb7ae80699d44a | now reads every mapping (request 5's hub profile lost all past the 1024th) |
| xlat.sh | 580174d36576a6eff073e6741a727da4 | same as request 3 |
| push-xlat.sh | d503e2c227e0aa7df142273ec5604036 | same as request 3 |

(xbcompat.xlat is xbcompat/ at commit 04edcd33, built in the cloud with
RENDERER=native.)

Thanks for request 5. What it showed and what changed since:

- The hub came up (260 s route) at 11.2-11.6 fps, then sank to ~8 over
  two minutes; box86 holds ~9.9.
- XLAT_SAFE_FLAGS=1 drew the characters right, so one of xlat's flag
  shortcuts breaks them. This build can name the shortcut: with
  XLAT_FLAG_CHECK=1 it reports each place where code read a flag the
  code before it left undefined, and XLAT_SAFE_FLAGS can now turn off one
  shortcut at a time.
- In the hub, sound mixing took ~80% of a core (dsmix_adpcm1_vp on three
  threads), with the title's calls waiting on the mixer's lock. Voices
  turned all the way down are now advanced without being mixed.
- xbprof showed the JIT and the libraries as [unmapped] in the hub: it
  stopped reading /proc/PID/maps at 1024 entries. The new xbprof reads
  them all.

## 1. Install

/etc/default/xbox: keep every renderer setting box86 runs with now
(XBCOMPAT_RENDER_WORKER, XBCOMPAT_ASYNC_DRAWS, XBCOMPAT_LATCH_TICKS,
XBCOMPAT_LOG_FPS and the rest) and add, set the same way, XLAT_PERF_MAP=1,
XLAT_STATS=60 and XLAT_FLAG_CHECK=1. Nothing else from request 5. On the
PC, from bootani/pi/bench:

    PI=192.168.1.128 sh push-xlat.sh

If the dashboard doesn't come up, it puts box86 back by itself. Its report
is xlat.txt next to the script. The log should have the lines
`D3D: native renderer bridge ABI 4, 312 exports, linked in` and
`xlat: flag check on (each block checks the flags it reads)`.

## 2. Flag check in the hub

Drive to the underground hub (~/Downloads/route-to-hub.sh) and stand
still. Screenshot (touch /tmp/xbshot.req) as soon as the hub is up, wait
60 s, and keep every line starting `xlat: flag check:` from the whole run
(dashboard included), plus one hub fps line.

## 3. Which shortcut

Remove XLAT_FLAG_CHECK. Then try each of these in turn, set the same way
(one at a time, removing the previous one), restarting xbox and driving
back to the hub each time:

    XLAT_SAFE_FLAGS=ret
    XLAT_SAFE_FLAGS=call
    XLAT_SAFE_FLAGS=jmp
    XLAT_SAFE_FLAGS=look
    XLAT_SAFE_FLAGS=nzcv

Do all five even if an early one fixes the characters (more than one may).
For each keep the log's `xlat: flags kept at ...` line, a hub screenshot,
one fps line, and whether the characters draw right. If Phantom Dust
crashes or hangs, keep the last 80 lines of the log and go on. Remove
XLAT_SAFE_FLAGS when done.

## 4. Plain hub, three minutes

Restart xbox, drive to the hub, screenshot when it's up. From that moment
keep every `D3D: ... fps` line and every `DSound:` line for three minutes
(the fps sank last time; this shows whether the sound voices grow with
it). At 60 s, at the same time:

    /root/xbprof $(pidof xbcompat.arm) 30 1000 60 > /tmp/xbprof-hub.txt
    sh /root/clocks.sh 30 > /tmp/clocks-hub.txt

and keep every `xlat:` line from the three minutes.

## 5. Sound mixed the old way

Add XBCOMPAT_MIX_SILENT=1 (mixes the inaudible voices too, as before),
restart, hub, stand still for two minutes, and keep every fps and
`DSound:` line from the hub. Then remove it.

## 6. Leave and report

    PI=192.168.1.128 sh push-xlat.sh undo

remove XLAT_PERF_MAP, XLAT_STATS, XLAT_FLAG_CHECK, XLAT_SAFE_FLAGS and
XBCOMPAT_MIX_SILENT from /etc/default/xbox, restart xbox, and check the
dashboard is back on box86.

Send the results to the cloud session in parts of at most 60 KB, each
starting `request 6 part i/N`:

    request 6: ok / problems, how long the route to the hub took
    xlat.txt (whole)
    step 2: every flag check line, the fps line, characters right or not
    step 3: for each setting, the flags kept line, fps line, characters right or not
    step 4: fps and DSound lines, xbprof-hub.txt and clocks-hub.txt (whole), the xlat: lines
    step 5: fps and DSound lines
    log tails if anything failed

For each screenshot, say in a line what it shows. If a step 3 try draws
the characters right, also send its hub screenshot as a 320x240 JPEG,
base64, in a part of its own
(`convert hub.bmp -resize 320x240 -quality 70 jpg:- | base64 -w0`).

Then keep watching the branch for request 7.
