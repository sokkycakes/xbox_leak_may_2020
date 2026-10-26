# push-usbaudio

USB sound cards on the Sion (the Chromebox's own jack has no driver; a VGA
output carries no sound). `snd-rawmidi.ko`, `snd-usbmidi-lib.ko` and
`snd-usb-audio.ko` built from vanilla 6.18.7 with the Sion's fragments plus
`CONFIG_SND_USB_AUDIO=m` (vermagic `6.18.7 SMP preempt mod_unload`), stripped.

Before loading, check `usbaudio.undef` against the Sion's exports:

    ssh sion "grep ' __ksymtab_' /proc/kallsyms" | sed 's/.* __ksymtab_//' | sort -u > sion.exports
    comm -23 usbaudio.undef sion.exports       # empty: they load

Install: copy this folder to the Sion and run `sh sion-install.sh` (NOW=1
restarts the dashboard so the sound moves at once). xbox-env then sets
AUDIODEV=plughw:CARD=<the USB card's id>,DEV=0 whenever a USB card is
plugged in at dashboard start, else HDMI as before; wincart-run's dmix
plays to the same card.
