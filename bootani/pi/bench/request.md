# Pi bench request 10

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat.xlat | adf378b0f28e864b6dba86cf924e5149 | new: less CPU work per draw (below) |
| pistat | 04736e5f7831eb4a6c14fd3836c533e2 | same as requests 8 and 9 |
| xbprof | 14e38d6997c96a87bbdb7ae80699d44a | same (installed, not used) |
| xlat.sh | 78e8f973b0df41a7dfb5e9c37741c786 | same as request 8 |
| push-xlat.sh | b0227e9de12bba813d6c5551b38dbc0a | same as request 8 |

Thanks for request 9. With no draw reaching the GPU the hub ran at 18.7
fps instead of 10.5, and the GPU-side what-ifs (no back-buffer views, flat
pixel shaders) gained under 1 fps. So the time goes to the CPU handing the
GPU each of the ~648 draws a frame: about 36 ms a frame inside Mesa's VC4
driver and 44 ms in xbcompat's own preparation, on the one render thread.
The flush mode wedged the GPU until the reboot; it is out of this and every
later request.

This build cuts that per-draw work in ways that apply to every game:

- a vertex program's constants go up only where rows were written since
  the program last drew, instead of everything between the first and last
  changed row (Mesa copies and re-sends whatever it is given);
- attribute pointers that are the same as the last draw's are not sent
  again (Mesa checks each one in full before finding nothing changed);
- the whole program is built for the Pi 3's Cortex-A53 with the hardware
  divide instruction, which plain ARMv7 builds replace with a library call.

`XBCOMPAT_WHATIF=cuts` turns the first two off and on again in turns, 10 s
each, so one session measures them with the heat affecting both alike. The
picture is right in both. The third can't be switched at run time; step 3
compares it with request 8's run from a cool Pi.

## 1. Install

The Pi is on box86 now. /etc/default/xbox: keep it as it is (no XLAT_*,
PROFILE_WORK or WHATIF settings). On the PC, from bootani/pi/bench:

    PI=192.168.1.128 sh push-xlat.sh

Check the md5s in xlat.txt.

## 2. Cool down

    /etc/init.d/S98xbox stop
    /root/pistat 600 > /tmp/pistat-cool.txt &

Check no xbcompat process is left. Leave the Pi idle until pistat's temp
column reads 60 C or less, or 10 minutes have passed, then stop pistat.

## 3. Hub, five minutes, as in request 8

    /root/pistat 1200 > /tmp/pistat-hub.txt &
    /etc/init.d/S98xbox start

Drive to the underground hub (~/Downloads/route-to-hub.sh) and stand still
for five minutes from hub-up. Keep every `D3D: ... fps` line from hub-up
on with its log timestamp, and note when the hub came up. Stop pistat.

## 4. Hub with the cuts toggling, five minutes

/etc/default/xbox: add XBCOMPAT_PROFILE_WORK=1 and XBCOMPAT_WHATIF=cuts,
written the same way as the other XBCOMPAT_ settings there. Then:

    /root/pistat 1200 > /tmp/pistat-cuts.txt &

restart xbox, drive to the hub and stand still for five minutes from
hub-up. Keep every log line containing `D3D` from hub-up on, whole: a
`D3D what-if:` line says whether the report below it ran with the cuts on
or off. Stop pistat.

If Phantom Dust crashes, hangs or draws wrongly in either step, keep the
last 80 lines of the log and say what looked wrong.

## 5. Leave and report

Remove XBCOMPAT_PROFILE_WORK and XBCOMPAT_WHATIF from /etc/default/xbox,
then

    PI=192.168.1.128 sh push-xlat.sh undo

It restarts xbox; check the dashboard is back on box86.

Send the results to the cloud session in parts of at most 60 KB, each
starting `request 10 part i/N`:

    request 10: ok / problems, the cool-down's end temperature, the log
                timestamps when each hub came up
    xlat.txt (whole)
    step 3: the fps lines, and pistat-hub as in request 8: header lines,
            10 s summaries in log time, last rows
    step 4: the D3D lines (whole), and pistat-cuts the same way

Then keep watching the branch for request 11.
