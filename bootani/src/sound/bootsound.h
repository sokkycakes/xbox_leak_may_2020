/*
 *  bootsound.h - portable version of the original Xbox boot sound
 *  (xbox/private/ntos/ani2: the "SOS" sequencer driving MCPX voices).
 *
 *  The sequencer (sos.c, proc.c, evf.c, cf.c, globals.c, stboot.c) is the
 *  original code; dev.c talks to a small software voice emulation
 *  (dsound_soft.c) instead of the MCPX hardware.
 *
 *  Usage:
 *      BootSound_Start();                       // once
 *      BootSound_Render(buf, frames);           // from the audio callback
 *      BootSound_Restart();                     // when the animation loops
 *      BootSound_Stop();                        // once
 *
 *  None of these functions are thread-safe with respect to each other.  If
 *  Render runs on an audio thread, serialise Start/Stop/Restart with it
 *  (e.g. by locking the audio device) - just as the original kernel code
 *  relied on the 5 ms DPC not running concurrently with itself.
 */
#ifndef BOOTSOUND_H
#define BOOTSOUND_H

#ifdef __cplusplus
extern "C" {
#endif

#define BOOTSOUND_SAMPLE_RATE   48000
#define BOOTSOUND_TICK_FRAMES   240     /* 5 ms sequencer tick at 48 kHz */

void BootSound_Start(void);            /* init sequencer + voices (like original) */
void BootSound_Stop(void);             /* cleanup */
void BootSound_Restart(void);          /* put_fifo(1): restart the boot tune */
/* Render interleaved stereo int16 PCM at 48000 Hz.  Internally advances the
   sequencer by calling  system_clock_music++; sos_main();  once per 5 ms of
   rendered audio (240 frames), exactly like the original 5 ms DPC timer
   (which fired immediately when started, then every 5 ms). */
void BootSound_Render(short *out_stereo, int frames);

#ifdef __cplusplus
}
#endif

#endif /* BOOTSOUND_H */
