# Pi 3 audio output and Dashboard volume

Measured 2026-10-10 UTC, following renderer commit 56d801b.

## Three separate faults

1. The running kernel had snd_bcm2835.enable_headphones=0. Only vc4hdmi
   appeared in /proc/asound/cards. SDL's ALSA open failed with error 524 and
   xbcompat fell back to its dummy audio driver. Reloading the unused
   snd_bcm2835 module with enable_headphones=1 enabled the Headphones card.
   Selecting AUDIODEV=plughw:CARD=Headphones,DEV=0 then opened real stereo
   playback at 48 kHz. The user confirmed sound from the jack.
2. A 512-frame host period caused frequent ALSA underruns (354 messages in
   the initial roughly 40-second playback run). A 1024-frame period, keeping
   512-frame internal mixing chunks, produced no reported underruns during
   more than two minutes of diagnostic testing and the subsequent clean run.
3. The Dashboard was quieter on x86 as well. Its running code calls the
   unversioned SetMixBinHeadroom with bin=0x7fffffff and headroom=0. This is
   the old DSMIXBIN_VALID mask; xbcompat treated it as an out-of-range modern
   index and rejected it. The default one-bit attenuation remained applied,
   reducing each affected output's amplitude by half (about 6.02 dB).

## Volume correction

The original Dashboard source, private/ui/xapp/DSound.cpp, explicitly applies
12 dB of voice headroom and requests zero headroom on all mix bins. The old
DirectSound compatibility implementation in
private/windows/directx/dsound/dsound/dsapi.old.cpp expands a mask into
individual SetMixBinHeadroom calls. The current implementation uses indices.

xbcompat now accepts the unambiguous 0x7fffffff all-bin mask at the unversioned
entry and provides explicit _v1 C and C++ exports for arbitrary legacy masks.
Small unversioned values keep their modern index meaning, and other invalid
indices remain errors. The Dashboard's requested voice headroom and per-voice
volume remain intact. This is an API compatibility correction, not a global
volume boost for games. It establishes a 6 dB error; it does not establish that
all remaining Dashboard/game loudness differences are bugs.

A temporary probe showed ambient mix peaks around 0.02–0.04 and RMS around
0.005–0.01 before the correction, with voice gains 0.07063 and 0.12559. Those
values include the Dashboard's 12 dB voice headroom plus the unintended
one-bit mix-bin attenuation. The final build removes the probe.

## Buffer configuration and verification

XBCOMPAT_AUDIO_FRAMES accepts 512, 1024, 2048 or 4096. The default remains 512;
the Pi launcher defaults to 1024 unless already overridden. At 48 kHz this
changes the host period from 10.7 to 21.3 ms, and the observed ALSA buffer from
1024 to 2048 frames. It adds buffering latency. Internal mixing still uses
512-frame blocks; sample-clock extrapolation uses the actual SDL callback
period rather than retaining a 512-frame cap.

The DirectSound test suite passes 233 checks at both the default period and
1024 frames. New sample-level checks cover the all-bin mask's exact factor-of-two
amplitude correction, preserved voice headroom, modern right-bin index versus
legacy left-bin mask semantics, and invalid parameters. The standalone test's
stubs were updated for the current runtime interfaces. Both built-in i386 and
native-renderer clients build successfully.

The clean native-renderer client is running on the Pi as
/tmp/xbcompat-native-test.if8zCJ/xbcompat-audio-fixed, using the mipmap-corrected
ARM library in the same directory's mip subdirectory. ALSA reports the
Headphones PCM as RUNNING, stereo S16_LE at 48 kHz, with 1024-frame periods
and a 2048-frame buffer. The analog mixer is unmuted at 0 dB. No headphone
volume amplification was applied. The 109-draw menu stays around 30.4–31.0 FPS
with 4x MSAA and real audio active. Earlier roughly 33 FPS measurements used
the dummy audio fallback; they were not audible-playback results.

## Persistence

The Pi's stored config.txt already contained dtparam=audio=on even though the
running kernel had disabled headphone parameters; the origin of that mismatch
was not established. The boot cmdline now explicitly appends
snd_bcm2835.enable_headphones=1, preserving all other existing arguments.
The previous file is saved as cmdline.txt.before-audio on the boot partition.
The startup environment defaults XBCOMPAT_AUDIO_FRAMES to 1024 on Pi; its
previous version is /usr/lib/xbox/xbox-env.before-audio. The repository's Pi
image configuration carries the same settings.

No reboot was performed, so persistence of the effective driver parameter still
needs verification after a future boot. The fixed runtime binary remains an
isolated temporary test; the installed runtime is unchanged. The volume fix
applies to x86 builds too once rebuilt from this branch. The user has not yet
confirmed the corrected perceived loudness relative to games.
