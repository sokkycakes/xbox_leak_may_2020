# push-soundrouter

All Sion sound goes into an ALSA loopback card. `sion-audiod` mixes it and plays it on:

- the main output: the first USB sound card (headset, USB sound card), or the TV (HDMI) when none is plugged in. It follows hot-plugging, so a headset plugged in mid-game takes over within a second. `MAIN=` in /etc/default/sion-audio forces one.
- every JUA365 adapter (ALSA card `Trigger6`) at the same time, so whatever is on the adapter's HDMI hears the game. `MIRROR=0` turns that off.

Files: snd-aloop.ko, trigger6.ko (rev d), sion-audiod, S07sion-audiod, sion-install.sh.
Install: run `sh sion-install.sh` as root on the Sion. `NOW=1` restarts the Xbox session right away when no game is running; otherwise the sound moves at the next session start.
Off switch: `touch /etc/sion/sion-audiod.off` (then xbox-env falls back to the old USB/HDMI choice). Undo: the *.pre-router backups.
