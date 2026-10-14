# Pi bench request 4

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| xbcompat.xlat | 4937f02ce3b962f6e2e5e64038b2dfac | the translator build again, newer: SSE arithmetic translated, faster x87 compares and loops |
| xlat.sh | 580174d36576a6eff073e6741a727da4 | same as request 3 |
| push-xlat.sh | d503e2c227e0aa7df142273ec5604036 | same as request 3 |
| xbprof | be0a3503bc9c5d1f130208244922ec2e | same as request 3 |

(xbcompat.xlat is xbcompat/ at commit be4304d2, built in the cloud.)

Goal: request 3's Phantom Dust hub measurement with this newer translator
build, so the two builds compare directly.

If requests 2 and 3 haven't run yet, run them first, in order: request 2's
files are at commit 651d6364 and request 3's at commit 6f0550ff. Their
results come before this one.

## 1. Install

/etc/default/xbox as for request 3: XLAT_PERF_MAP=1 and XLAT_STATS=60, no
BOX86_PROFILE, no XLAT_COUNT. Add XLAT_SAFE_FLAGS=1 only if request 3
needed it for Phantom Dust. On the PC, from bootani/pi/bench:

    PI=192.168.1.128 sh push-xlat.sh

If the dashboard doesn't come up, it puts box86 back by itself. Its
report is xlat.txt next to the script.

## 2. Phantom Dust hub

Exactly request 3's step 3: drive to the underground hub, stand still,
wait 30 s, then at the same time

    /root/xbprof $(pidof xbcompat.arm) 30 1000 60 > /tmp/xbprof-hub.txt
    sh /root/clocks.sh 30 > /tmp/clocks-hub.txt

Note the fps lines over that minute and keep the `xlat:` lines from the
last two `xlat: last 60 s:` groups. If Phantom Dust crashes, hangs or draws
wrong, keep the last 80 lines of the log and a screenshot (touch
/tmp/xbshot.req), and skip to step 4.

## 3. Hottest code in the hub

Exactly request 3's step 3b: XLAT_COUNT=/tmp/xlathot (mkdir -p it on the
Pi), restart xbox, back to the hub, stand still for 70 s, copy
/tmp/xlathot/hot.txt to the PC.

## 4. Leave and report

    PI=192.168.1.128 sh push-xlat.sh undo

remove XLAT_PERF_MAP, XLAT_STATS, XLAT_COUNT and XLAT_SAFE_FLAGS from
/etc/default/xbox, restart xbox, and check the dashboard is back on
box86. Send one message to the cloud session:

    request 4: ok / problems
    xlat.txt (whole)
    hub fps lines
    xbprof-hub.txt, clocks-hub.txt (whole)
    the hub's xlat: lines, hot.txt (whole)
    log tail and screenshot if Phantom Dust failed

Then keep watching the branch for request 5.
