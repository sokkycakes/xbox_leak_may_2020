/*
 * dsound_soft.h - a tiny software stand-in for the subset of the (August 2001
 * era) Xbox DirectSound / MCPX voice processor API that the boot sound
 * sequencer (dev.c) uses.
 *
 * Only what dev.c / stboot.c need is provided: 16-bit mono 48 kHz buffers,
 * SetBufferData / SetLoopRegion / SetCurrentPosition, Play / StopEx,
 * SetPitch, SetVolume, SetMixBinVolumes (mask + volume array form),
 * SetEG (amplitude + multi-function envelopes) and SetFilter (DLS2 mode).
 *
 * The API names used by dev.c (IDirectSoundBuffer_SetPitch, ...) are macros
 * that map onto dsoft_* functions, so no symbol clashes with a real
 * DirectSound library can happen at link time.
 *
 * Portable C99, no Windows headers.
 */
#ifndef DSOUND_SOFT_H
#define DSOUND_SOFT_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Win32-ish scalar types (only inside the sound library) ---- */
typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef uint16_t WORD;
typedef int      HRESULT;

#define DS_OK           0
#define E_FAIL          (-1)
#define FAILED(hr)      ((hr) < 0)
#define SUCCEEDED(hr)   ((hr) >= 0)
#define ZeroMemory(p, n) memset((p), 0, (n))

/* ---- formats / buffer description ---- */
#define WAVE_FORMAT_PCM 1

typedef struct WAVEFORMATEX {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
} WAVEFORMATEX, *LPWAVEFORMATEX;

typedef struct DSBUFFERDESC {
    DWORD           dwSize;
    DWORD           dwFlags;
    DWORD           dwBufferBytes;
    LPWAVEFORMATEX  lpwfxFormat;
    DWORD           dwMixBinMask;       /* Aug-2001 API: mixbin bit mask */
    DWORD           dwInputMixBinMask;
} DSBUFFERDESC;

/* mixbins, Aug-2001 (mask) form */
#define DSMIXBIN_FRONT_LEFT     0x00000001
#define DSMIXBIN_FRONT_RIGHT    0x00000002
#define DSMIXBIN_FRONT_CENTER   0x00000004
#define DSMIXBIN_LOW_FREQUENCY  0x00000008
#define DSMIXBIN_BACK_LEFT      0x00000010
#define DSMIXBIN_BACK_RIGHT     0x00000020
#define DSMIXBIN_FXSEND_0       0x00000800

/* Play / Stop flags */
#define DSBPLAY_LOOPING             0x00000001
#define DSBPLAY_FROMSTART           0x00000002
#define DSBSTOPEX_IMMEDIATE         0x00000000
#define DSBSTOPEX_ENVELOPE          0x00000001
#define DSBSTOPEX_RELEASEWAVEFORM   0x00000002

/* volume / pitch ranges */
#define DSBVOLUME_MIN   (-10000)
#define DSBVOLUME_MAX   0
#define DSBPITCH_MIN    (-32767)
#define DSBPITCH_MAX    8191
#define DSBHEADROOM_DEFAULT_2D 600      /* mB, applied by SetVolume */
#define DSHEADROOM_DEFAULT     1        /* mixbin headroom, in 6 dB steps */

/* ---- envelopes ---- */
#define DSEG_MULTI              0x00000000
#define DSEG_AMPLITUDE          0x00000001
#define DSEG_MODE_DISABLE       0x00000000
#define DSEG_MODE_DELAY         0x00000001
#define DSEG_MODE_ATTACK        0x00000002
#define DSEG_MODE_HOLD          0x00000003

typedef struct DSENVELOPEDESC {
    DWORD dwEG;          /* DSEG_AMPLITUDE or DSEG_MULTI                     */
    DWORD dwMode;        /* DSEG_MODE_*                                      */
    DWORD dwDelay;       /* 512-sample blocks before attack                  */
    DWORD dwAttack;      /* attack length, 512-sample blocks                 */
    DWORD dwHold;        /* hold length, 512-sample blocks                   */
    DWORD dwDecay;       /* decay length, 512-sample blocks                  */
    DWORD dwRelease;     /* release length, 512-sample blocks                */
    DWORD dwSustain;     /* u.8 sustain level (0..255)                       */
    LONG  lPitchScale;   /* s.7 : 0x7f ~ +1 octave   (multi EG only)         */
    LONG  lFilterCutOff; /* s3.4: 0x80 = -8 oct, 0x7f ~ +8 oct (multi EG)    */
} DSENVELOPEDESC, *LPDSENVELOPEDESC;
typedef const DSENVELOPEDESC *LPCDSENVELOPEDESC;

/* ---- filter ---- */
#define DSFILTER_MODE_BYPASS    0x00000000
#define DSFILTER_MODE_DLS2      0x00000001
#define DSFILTER_MODE_PARAMEQ   0x00000002
#define DSFILTER_MODE_MULTI     0x00000003

typedef struct DSFILTERDESC {
    DWORD dwMode;
    DWORD dwQCoefficient;
    DWORD adwCoefficients[4];
} DSFILTERDESC, *LPDSFILTERDESC;
typedef const DSFILTERDESC *LPCDSFILTERDESC;

/* ---- objects ---- */
typedef struct DSoftBuffer *LPDIRECTSOUNDBUFFER8;
typedef struct DSoftDevice *LPDIRECTSOUND8;

HRESULT dsoft_Create(void *guid, LPDIRECTSOUND8 *ppDS, void *outer);
HRESULT dsoft_ReleaseDevice(LPDIRECTSOUND8 pDS);
HRESULT dsoft_CreateBuffer(const DSBUFFERDESC *desc, LPDIRECTSOUNDBUFFER8 *ppBuf);
HRESULT dsoft_Release(LPDIRECTSOUNDBUFFER8 b);
HRESULT dsoft_SetBufferData(LPDIRECTSOUNDBUFFER8 b, const void *data, DWORD bytes);
HRESULT dsoft_SetLoopRegion(LPDIRECTSOUNDBUFFER8 b, DWORD startBytes, DWORD lenBytes);
HRESULT dsoft_SetCurrentPosition(LPDIRECTSOUNDBUFFER8 b, DWORD posBytes);
HRESULT dsoft_Play(LPDIRECTSOUNDBUFFER8 b, DWORD r1, DWORD r2, DWORD flags);
HRESULT dsoft_StopEx(LPDIRECTSOUNDBUFFER8 b, int64_t rtTimeStamp, DWORD flags);
HRESULT dsoft_SetPitch(LPDIRECTSOUNDBUFFER8 b, LONG pitch);
HRESULT dsoft_SetVolume(LPDIRECTSOUNDBUFFER8 b, LONG volume);
HRESULT dsoft_SetMixBinVolumes(LPDIRECTSOUNDBUFFER8 b, DWORD mask, const LONG *vols);
HRESULT dsoft_SetEG(LPDIRECTSOUNDBUFFER8 b, LPCDSENVELOPEDESC env);
HRESULT dsoft_SetFilter(LPDIRECTSOUNDBUFFER8 b, LPCDSFILTERDESC f);

/* Mix every live buffer of the (single) device into out[] as interleaved
   stereo float, 48 kHz, full scale = 32768.0.  out is overwritten. */
void    dsoft_Render(float *out_stereo, int frames);
/* Reset global mixer state (reverb tails, DC blocker). */
void    dsoft_ResetMixer(void);

/* DirectSound-style names used by the original dev.c */
#define DirectSoundCreate                   dsoft_Create
#define DirectSoundCreateBuffer             dsoft_CreateBuffer
#define IDirectSound_Release                dsoft_ReleaseDevice
#define IDirectSoundBuffer_Release          dsoft_Release
#define IDirectSoundBuffer_SetBufferData    dsoft_SetBufferData
#define IDirectSoundBuffer_SetLoopRegion    dsoft_SetLoopRegion
#define IDirectSoundBuffer_SetCurrentPosition dsoft_SetCurrentPosition
#define IDirectSoundBuffer_Play             dsoft_Play
#define IDirectSoundBuffer_StopEx           dsoft_StopEx
#define IDirectSoundBuffer_SetPitch         dsoft_SetPitch
#define IDirectSoundBuffer_SetVolume        dsoft_SetVolume
#define IDirectSoundBuffer_SetMixBinVolumes dsoft_SetMixBinVolumes
#define IDirectSoundBuffer_SetEG            dsoft_SetEG
#define IDirectSoundBuffer_SetFilter        dsoft_SetFilter

#ifdef __cplusplus
}
#endif

#endif /* DSOUND_SOFT_H */
