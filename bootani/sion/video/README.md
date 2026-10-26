# 15 kHz 480i out of the Sion

For a VGA-to-composite transcoder: the TV gets the native NTSC signal, 480
lines interlaced, 15.734 kHz lines, 59.94 fields a second.

## The mode

BT.601 draws an NTSC line as 720 pixels of an 858-pixel line at 13.5 MHz.
i915 won't send HDMI below 25 MHz (`intel_hdmi.c`, `MODE_CLOCK_LOW`), so the
Sion draws each line four times as finely: **2880x480i, 3432-pixel lines at
54 MHz**. The line is just as long and the syncs land in the same places, so
on the VGA side (which has no pixels) the signal is the same 15 kHz NTSC.
The Kaby Lake's display engine does interlace on HDMI and DisplayPort
(display version 9; i915 stops at 11). It can't scale a plane in an
interlaced mode, so every program draws the full 2880 width itself; they all
stretch their picture to the mode already (xbcompat, the Guide, gamescope
with `-S stretch`, xbox-av), and bootani and the DS runtime's TV output now
treat any 480- or 576-line mode as a 4:3 TV.

| | pixels | line | clock | lines/field | sync |
|---|---|---|---|---|---|
| 2880x480i | 2880 (front 76, sync 248, back 228) | 3432 = 15.734 kHz | 54 MHz | 240 + 22.5 (front 4, sync 3) | both negative |

These are exactly CEA-861's VIC 10, so i915 sends limited-range RGB
(16-235), as the HDMI-to-AV box got its 480p. Full range (an 80-pixel front
porch, so not VIC 10) came out over-saturated through the transcoder.

## Getting the mode to every program

No VGA adapter reports a mode like this, and a transcoder may give no EDID at
all, so `sion-video` puts its own EDID (`crimson-480i.edid`, made by
`mkedid.py`) on the connector through DRM's `edid_override` in debugfs. The
connector then lists 2880x480i (and 720x480p, VIC 2), shows as connected
even with no EDID of its own, and keeps HDMI audio on (the EDID has an HDMI
block and stereo LPCM), for adapters with a 3.5 mm jack: the Chromebox's own
headphone jack is on the I2S codec this image leaves out. `KMS_MODE` in
/etc/default/kms becomes `2880x480i`, which bootani, SDL (xbcompat), xbox-av,
wincart-run (gamescope) and the DS runtime all read.

```
sion-video                    status: setting, this boot's decision, connectors
sion-video 480i | 480p | auto choose; applies at the next boot or `sion restart dashboard`
sion-video remember-av        the display plugged in now is the HDMI-to-AV box
```

`auto` (the default) gives the remembered HDMI-to-AV box 480p and anything
else 480i, so plugging the old box back in gets its picture back at the next
boot. The setting is in /data/sion/video.conf, with a copy on the EFI
partition (sion/video.conf) for S00bootani, which runs before /data is
mounted. `VIDEO_CONNECTOR=DP-1` there picks another port.

## When it applies

- Image (S00bootani, `sion-video early`): before the boot animation, so the
  whole boot is 15 kHz.
- Live push (push-480i, until the next image): xbox-env runs `sion-video
  apply` when the dashboard starts. The boot animation still plays in 480p
  (nothing on the transcoder for ~10 s), then the dashboard comes up in 480i.

## Tested

In the cloud (no Intel GPU): the real 6.18.7 kernel in QEMU with the Sion
image's own userland, on QEMU's std VGA (bochs-drm, which like i915 reads
the EDID, and so the override, on every probe; patched only to allow
interlaced modes, as i915 does). With the EDID on, the kernel builds exactly
`"2880x480i": 60 54000 2880 2960 3208 3432 480 488 494 525 0x48 0x1a`
(54 MHz, 15.734 kHz, interlaced, negative syncs); xbox-av and bootani pick
it from KMS_MODE and bootani scans out 2880x480 interlaced; 480p and auto
(remembered box gets 480p, anything else 480i) switch back and forth. The DS
TV output at 2880 wide is pixel-for-pixel its 720 picture, four times over.

Not tested: i915 itself sending it, and whether a given HDMI-to-VGA adapter
passes 15 kHz interlaced timing (most are plain DACs; ones with a scaler
chip may not).
