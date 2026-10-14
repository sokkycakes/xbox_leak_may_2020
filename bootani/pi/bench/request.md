# Pi bench request 8

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat.xlat | 88864fa5c5446fb6236728c8af8719bc | same build as request 7 |
| pistat | 04736e5f7831eb4a6c14fd3836c533e2 | new: reads the Pi's real clocks (xbcompat/tools/pi/pistat.c) |
| xbprof | 14e38d6997c96a87bbdb7ae80699d44a | same as request 7 (installed, not used) |
| xlat.sh | 78e8f973b0df41a7dfb5e9c37741c786 | now also installs /root/pistat |
| push-xlat.sh | b0227e9de12bba813d6c5551b38dbc0a | now also sends pistat |

Thanks for request 7. It showed that the hub's slowdown isn't the game
doing more: late in the run the sound threads mixed exactly the same voices
as early on (3.05M voice frames a second) for 17% more CPU time, and the
render worker drew the same ~648 draws a frame for ~18% more CPU time per
frame. The same work getting slower in every thread means the Pi itself is
running slower. The temperature sat at 83-85 C in both windows, and 85 C
is where the Pi's firmware starts lowering its clocks on its own. clocks.sh
can't see that: the kernel only knows the 1200 MHz it asked for.

pistat asks the firmware directly. Once a second it prints the ARM, core,
V3D and SDRAM clocks the firmware measures, its throttle flags and the
temperature, plus the ARM clock timed from a known instruction loop, with
the same monotonic seconds xbcompat's log uses. It costs about 3 ms of one
core a second. This request records it from a cooled-down Pi through the
route and five minutes in the hub.

## 1. Install

The Pi is on box86 now. /etc/default/xbox: keep it as it is (no XLAT_*
settings). On the PC, from bootani/pi/bench:

    PI=192.168.1.128 sh push-xlat.sh

If the dashboard doesn't come up, it puts box86 back by itself. Its report
is xlat.txt next to the script; check the md5s in it, pistat included.

## 2. Cool down

    /etc/init.d/S98xbox stop
    /root/pistat 600 > /tmp/pistat-cool.txt &

Check no xbcompat process is left. Leave the Pi idle until pistat's temp
column reads 60 C or less, or 10 minutes have passed, then stop pistat.

## 3. Hub, five minutes, pistat running throughout

Start pistat first, then the dashboard:

    /root/pistat 1200 > /tmp/pistat-hub.txt &
    /etc/init.d/S98xbox start

Drive to the underground hub (~/Downloads/route-to-hub.sh) and stand still.
Keep every `D3D: ... fps` line and every `DSound:` line from the moment the
hub is up for five minutes, with their log timestamps, and note the log
timestamp at which the hub came up. Then stop pistat. No screenshot or
xbprof this time.

If Phantom Dust crashes or hangs, keep the last 80 lines of the log.

## 4. Leave and report

    PI=192.168.1.128 sh push-xlat.sh undo

It restarts xbox; check the dashboard is back on box86.

Send the results to the cloud session in parts of at most 60 KB, each
starting `request 8 part i/N`:

    request 8: ok / problems, how long the cool-down and the route took,
               the log timestamp when the hub came up
    xlat.txt (whole)
    pistat-cool.txt (whole)
    pistat-hub.txt (whole)
    step 3: the fps and DSound lines with timestamps, condensed as in
            request 7
    log tails if anything failed

Then keep watching the branch for request 9.
