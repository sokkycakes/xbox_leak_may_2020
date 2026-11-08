# push-levels: every app at the Guide's loudness

sion-audiod (the sound router) now knows which app each sound stream comes
from (the dashboard, Frolick, an Xbox game by title ID, the DS runtime, a
Windows game, the Windows games' Guide) and gives it ONE fixed volume so it
sits at the Guide's loudness (-19 LUFS). Not a compressor or limiter: the
volume of each app is measured once, over its first two minutes of sound,
saved in /data/sion-audio/levels and never changed after that. Before an
app is measured it plays at its kind's starting volume (Xbox apps +12 dB,
DS runtime -6 dB). The only thing touching the mix is a clip guard above
-1 dBFS.

The Guide inside an Xbox game shares the game's stream, so the kit turns the
Guide's sound files down 12 dB (the usual game volume is +12); the Windows
games' Guide gets the 12 dB back in the router.

- Install: `sh sion-install.sh` as root on the Sion, with no game running.
- See what plays: `cat /run/sion-audiod.status`; a line a minute per app in
  /var/log/sion-audiod.log.
- Own numbers: edit /data/sion-audio/levels (`learned` -> `set` keeps yours),
  or GAIN_DASH= / GAIN_GAME= / GAIN_DS= ... and TARGET= in
  /etc/default/sion-audio. LEVELS=0 there plays every app as it is.
  Picked up within 5 s.
- Measure an app again: delete its line in /data/sion-audio/levels.
- Undo: `sh undo.sh`.

Source: bootani/sion/audio/sion-audiod.c (built on 4f400cea: all playback
devices of the USB headset, switches on, Trigger6 mirror kept).
