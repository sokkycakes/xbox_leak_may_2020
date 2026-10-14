# Pi bench request 2

Written by the cloud Claude session for the Linux PC session, under sokky's
standing approval. Measure only: nothing is built on the PC. Files are in
bootani/pi/bench at the commit this file was read from; check every md5.

| file | md5 | what |
|---|---|---|
| guest-report.py | e22c44d5098f0bb29c2189c1ad7b4cf1 | adds up a profile by game code / library code / xbcompat code (runs on the PC) |
| xbprof | e0453100f2b93e91e2f354d450ee37c0 | whole-process profiler (static armhf) |

Goal: how Phantom Dust's hub frame splits between the game's own x86 code,
xbcompat's x86 code (both run through box86), box86 itself and native
libraries (renderer, Mesa, libc, kernel). It decides what the cloud
session's Xbox translator has to be good at.

Don't use XBCOMPAT_PROFILE: its sampler hangs under box86. Use box86's
own BOX86_PROFILE, which never runs x86 code from its signal handler.

## 1. Setup

If /opt/xbcompat/bin/xbcompat-native.x86 isn't md5 adb2c39a6c27c2bbe5ccf0cab80d33d6
(the request 1 build, with the pad pipe), install request 1's kit first:
its files at commit f591eeaa3550998c874db6e0dcd44ee6497570e7, `sh push-vblank.sh`.

/etc/default/xbox: keep XBCOMPAT_RENDER_WORKER=1, XBCOMPAT_ASYNC_DRAWS=1,
the latch and work-profile settings, XBCOMPAT_PAD_PIPE=/tmp/xbpad and
XBCOMPAT_SHOT_ON_REQUEST=/tmp/xbshot, no BOX86_DYNAREC_ variants, and add,
set the same way as the others:

    BOX86_PROFILE=/tmp/box86prof.txt

Copy xbprof to /root/xbprof on the Pi if it isn't there. Restart xbox.

## 2. Measure in the hub

Drive Phantom Dust to the underground hub (the route from request 1, or
by pad pipe and screenshots) and stand still. Wait 30 s, then:

    /root/xbprof $(pidof xbcompat-native.x86) 30 1000 40 > /tmp/xbprof.txt

Wait 10 s more (box86 rewrites its file every 5 s), then copy to the PC:
/tmp/box86prof.txt, /tmp/xbprof.txt, the title's library map (the file
after `--hle` in `tr '\0' ' ' < /proc/$(pidof xbcompat-native.x86)/cmdline`)
and E:\Games\Phantom Dust\default.xbe (read only). On the PC:

    python3 guest-report.py box86prof.txt TITLE.map default.xbe xbcompat-native.x86

(xbcompat-native.x86 from request 1's commit, for addr2line; skip it if
addr2line isn't installed.) Also note the fps line from the log over the
same minute.

## 3. Leave and report

Remove BOX86_PROFILE, restart xbox. Send one message to the cloud session:

    request 2: ok / problems
    hub fps and ms per frame during the profile
    xbprof.txt (whole)
    guest-report.py output (whole)
    box86prof.txt (whole)

Then keep watching the branch for request 3.
