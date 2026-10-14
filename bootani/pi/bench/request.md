# Pi bench request 3

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat.xlat | 0b051a758f9723f1494591c896f1d3b0 | xbcompat built for ARM with its own Xbox translator (xlat), no box86 |
| xlat.sh | 580174d36576a6eff073e6741a727da4 | runs on the Pi: installs it, `sh /root/xlat.sh undo` puts box86 back |
| push-xlat.sh | d503e2c227e0aa7df142273ec5604036 | runs on the PC: sends the three files and runs xlat.sh |
| xbprof | be0a3503bc9c5d1f130208244922ec2e | whole-process profiler; now names xlat's code from /tmp/perf-PID.map |

(xbcompat.xlat is xbcompat/ at commit 89bf8171, built in the cloud.)

Goal: the first timing of the cloud's translator on the real Pi, against
box86: dashboard and Phantom Dust's underground hub, frame times and where
the CPU time goes.

If request 2 hasn't run yet, run it first (its files at commit
651d6364): it is the box86 side of this comparison.

## 1. Install

/etc/default/xbox: keep everything from request 2 except BOX86_PROFILE,
and add, set the same way as the others:

    XLAT_PERF_MAP=1
    XLAT_STATS=60

(XLAT_STATS makes the log print, every 60 s, the x86 instructions xlat
still hands to its interpreter: lines starting with `xlat:`.)

On the PC, from bootani/pi/bench:

    PI=192.168.1.128 sh push-xlat.sh

It keeps the Unicorn build as xbcompat.arm.unicorn, puts xbcompat.xlat in
its place, sets XBOX_CPU=unicorn (the launcher then runs that slot) and
restarts the dashboard. If the dashboard doesn't come up it puts box86
back by itself. Its report is xlat.txt next to the script.

## 2. Dashboard

On the main menu, untouched: the log's `D3D: ... fps` lines over 30 s
(/var/log/xbox-session.log or /var/log/xbox.log), then

    /root/xbprof $(pidof xbcompat.arm) 20 1000 40 > /tmp/xbprof-dash.txt

## 3. Phantom Dust hub

Drive Phantom Dust to the underground hub by the route from request 1
(pad pipe and screenshots) and stand still. Wait 30 s, then at the same
time:

    /root/xbprof $(pidof xbcompat.arm) 30 1000 60 > /tmp/xbprof-hub.txt
    sh /root/clocks.sh 30 > /tmp/clocks-hub.txt

and `vcgencmd get_throttled` if the image has vcgencmd. Note the fps lines
over that minute. Copy the title's library map (the file after `--hle` in
`tr '\0' ' ' < /proc/$(pidof xbcompat.arm)/cmdline`) to the PC.

Keep the `xlat:` lines the log printed while you stood in the hub (the
last two `xlat: last 60 s:` groups).

If Phantom Dust crashes, hangs or draws wrong on xlat, that's a result
too: keep the last 80 lines of the log and a screenshot (touch
/tmp/xbshot.req). Then add `XLAT_SAFE_FLAGS=1` (set the same way),
restart xbox and try the hub once more; report both tries. If it still
fails, go to step 4.

## 3b. Hottest code in the hub

Only if step 3 worked. Add, set the same way:

    XLAT_COUNT=/tmp/xlathot

`mkdir -p /tmp/xlathot` on the Pi, restart xbox, drive back to the hub
and stand still for 70 s. Copy /tmp/xlathot/hot.txt to the PC (it's
rewritten every 60 s with the blocks that ran most in that minute).
Frame times in this run don't count: the counters slow it down.

## 4. Leave and report

    PI=192.168.1.128 sh push-xlat.sh undo

remove XLAT_PERF_MAP, XLAT_STATS, XLAT_COUNT and XLAT_SAFE_FLAGS from
/etc/default/xbox, restart xbox, and check the dashboard is back on
box86. Send one message to the cloud session:

    request 3: ok / problems
    xlat.txt (whole)
    dashboard fps lines on xlat (and the box86 ones from request 2 or earlier)
    xbprof-dash.txt (whole)
    hub fps lines on xlat and on box86
    xbprof-hub.txt, clocks-hub.txt (whole), get_throttled
    the hub's xlat: lines, hot.txt (whole)
    the title's library map
    log tail and screenshot if Phantom Dust failed

Then keep watching the branch for request 4.
