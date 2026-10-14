# Pi bench request 9

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat.xlat | 88864fa5c5446fb6236728c8af8719bc | same build as requests 7 and 8 |
| pistat | 04736e5f7831eb4a6c14fd3836c533e2 | same as request 8 |
| xbprof | 14e38d6997c96a87bbdb7ae80699d44a | same (installed, not used) |
| xlat.sh | 78e8f973b0df41a7dfb5e9c37741c786 | same as request 8 |
| push-xlat.sh | b0227e9de12bba813d6c5551b38dbc0a | same as request 8 |

Thanks for request 8: it settled the slowdown. The firmware caps the ARM
from ~80 C, and over five minutes the clock fell 38% while the fps fell
only 20%. So roughly half of each hub frame doesn't speed up with the CPU
at all: it is the GPU, or work waiting on memory. This request finds out
which, with the renderer's built-in what-if modes, which leave one kind of
work out at a time in turn, 10 s each, over and over in one session. The
picture is wrong on purpose while they run. Because the modes take turns,
the heat affects them all alike, and pistat says what the clock was in
each turn.

(xbcompat's log prints CLOCK_MONOTONIC seconds modulo 100000, which is why
pistat's column read 200000 more. Subtract it as you did.)

## 1. Install

The Pi is on box86 now. On the PC, from bootani/pi/bench:

    PI=192.168.1.128 sh push-xlat.sh

Check the md5s in xlat.txt.

## 2. Hub with the what-if modes, five minutes

/etc/default/xbox: add XBCOMPAT_PROFILE_WORK=1 and XBCOMPAT_WHATIF=1,
written the same way as the other XBCOMPAT_ settings there. Then:

    /root/pistat 1200 > /tmp/pistat-a.txt &

restart xbox, drive to the underground hub (~/Downloads/route-to-hub.sh)
and stand still for five minutes from hub-up. Keep every log line
containing `D3D` from hub-up on, whole. With PROFILE_WORK on, each
five-second report is a few `D3D:` lines, and a `D3D what-if:` line says
which mode the report below it ran with. Stop pistat.

## 3. Hub with the flush settings, three minutes

In /etc/default/xbox change XBCOMPAT_WHATIF=1 to XBCOMPAT_WHATIF=flush
(keep PROFILE_WORK), then:

    /root/pistat 1200 > /tmp/pistat-b.txt &

restart xbox, drive to the hub, stand still three minutes, keep every `D3D`
line from hub-up on, stop pistat. This mode cycles how often the renderer
hands the GPU a part of the frame (never, every 64, 128 or 256 draws).

If Phantom Dust crashes or hangs in either, keep the last 80 lines of the
log.

## 4. Leave and report

Remove XBCOMPAT_PROFILE_WORK and XBCOMPAT_WHATIF from /etc/default/xbox,
then

    PI=192.168.1.128 sh push-xlat.sh undo

It restarts xbox; check the dashboard is back on box86.

Send the results to the cloud session in parts of at most 60 KB, each
starting `request 9 part i/N`:

    request 9: ok / problems, the log timestamps when each hub came up
    xlat.txt (whole)
    step 2: the D3D lines (whole), and pistat-a as in request 8: header
            lines, 10 s summaries in log time, last rows
    step 3: the same for its D3D lines and pistat-b

Then keep watching the branch for request 10.
