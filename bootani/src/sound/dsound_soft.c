/*
 * dsound_soft.c - software emulation of the handful of Xbox DirectSound /
 * MCPX voice-processor features used by the boot sound sequencer.
 *
 * What is modelled, and how faithfully (see also the notes in bootsound.h):
 *
 *  - Buffers: 16-bit signed mono PCM at 48 kHz (the only format dev.c
 *    creates).  Looping plays [LoopStart, LoopStart+LoopLength) forever,
 *    one-shot plays the whole buffer and then turns the voice off, exactly
 *    like CMcpxBuffer::PlayFromCurrent programs EBO/LBO.  Samples are read
 *    with linear interpolation (the MCPX interpolator is not documented).
 *
 *  - Play()/StopEx() state machine follows dsound/mcpbuf.cpp: Play() on an
 *    already active voice (including one that is in its release phase) only
 *    updates the loop flag, it does NOT retrigger; Play() on an inactive
 *    voice starts at the cached cursor (SetCurrentPosition) or 0 and
 *    restarts both envelopes.  StopEx(DSBSTOPEX_ENVELOPE) enters the release
 *    segment; the voice turns off when the amplitude envelope reaches zero.
 *
 *  - SetPitch: register value is s3.12 octaves relative to 48 kHz,
 *    step = 2^(pitch/4096), clamped to [-8, +2] octaves like the APU.
 *
 *  - SetVolume / SetMixBinVolumes: exact dsound arithmetic.  SetVolume
 *    subtracts the default 2D voice headroom (600 mB); for every mixbin the
 *    attenuation -(volume + mixbin volume) is converted to the u6.6 dB
 *    register with (att << 6) / 100, 0xFFF meaning mute.  The front-left and
 *    front-right mixbins go to the stereo output.  Mixbin headroom (default 1
 *    = 6 dB) is applied to the final mix.
 *
 *  - FXSEND_0 feeds the boot GP DSP image (dsp/fx/bootsnd.asm), which runs
 *    the Sensaura I3DL2 reverb with the "Bathroom" preset and writes it to the
 *    front L/R mixbins.  That reverb is APPROXIMATED here with a small 4-line
 *    feedback delay network tuned to the preset (RT60 1.49 s, HF ratio 0.54,
 *    7 ms reflections, 11 ms late delay, Room -10 dB).
 *
 *  - SetEG: both envelope generators run on 512-sample units exactly as the
 *    register docs specify (delay/attack/hold/decay/release lengths, u.8
 *    sustain).  Segment shapes: attack is a linear ramp, decay and release
 *    are exponential curves that land on sustain/zero at the end of the
 *    programmed length (the APU docs only say "decays exponentially").
 *    A length of 0 is an instantaneous segment.  The multi-function EG
 *    modulates pitch by lPitchScale (s.7, 0x7f ~ +1 octave) and the filter
 *    cutoff by lFilterCutOff (s3.4, 0x80 = -8 octaves).
 *
 *  - SetFilter (DSFILTER_MODE_DLS2): coefficient 0 is the cutoff as a signed
 *    s3.12 log2 of the Chamberlin state-variable coefficient
 *    f = 2*sin(pi*fc/48000) (see FreqToHardwareCoeff in the XDK SetFilter
 *    sample / dmsynth's g_nFilter table: 0x8000 ~ 30 Hz ... 0xFFFF ~ 8 kHz);
 *    coefficient 1 is the damping q = 10^(-resonance_dB/20) in 1.15
 *    (0x8000 = 0 dB).  We run a 2-pole Chamberlin SVF low-pass with those
 *    coefficients, which is what the numbers were designed for, but the
 *    exact MCPX fixed-point filter is not documented, so this is an
 *    approximation.  Cutoff values that wrap to the positive half (f > 1)
 *    are clamped to f = 1 (~8 kHz, i.e. "wide open").  Parameter changes are
 *    smoothed (the APU "tracks" towards target values).
 *
 *  - A 10 Hz DC blocker models the AC-coupled analog output (the sequencer's
 *    noise table is all-positive, i.e. carries a large DC offset).
 */
#include "dsound_soft.h"

#include <math.h>
#include <stddef.h>

#define DSOFT_MAX_BUFFERS   32
#define DSOFT_FS            48000
#define DSOFT_EG_UNIT       512     /* samples per envelope time unit */
#define DSOFT_FRAME         32      /* APU frame: control-rate update */

enum {
    EG_OFF = 0,
    EG_DELAY,
    EG_ATTACK,
    EG_HOLD,
    EG_DECAY,
    EG_SUSTAIN,
    EG_RELEASE
};

typedef struct SoftEG {
    DSENVELOPEDESC d;
    int      seg;
    uint32_t count;     /* samples remaining in current segment */
    float    value;     /* 0..1 */
    float    step;      /* attack increment */
    float    coef;      /* exponential coefficient for decay / release */
    float    target;    /* decay target (sustain) */
} SoftEG;

struct DSoftBuffer {
    int             used;
    const int16_t  *data;
    uint32_t        nsamp;
    uint32_t        loopStart;      /* samples */
    uint32_t        loopLen;        /* samples */
    DWORD           mixmask;
    LONG            binvol[32];     /* mB, per mixbin bit */
    LONG            volume;         /* mB, already including headroom */
    LONG            pitch;          /* s3.12 octaves */

    int             active;
    int             looping;
    int             noteoff;
    uint32_t        cached;         /* cached cursor, samples */
    double          pos;            /* sample position */

    SoftEG          ampEG;
    SoftEG          multiEG;

    DWORD           fmode;
    DWORD           fc0, fc1;
    float           fcCur;          /* smoothed log-coefficient (s3.12 units) */
    float           qCur;
    float           f, q;           /* per-frame SVF coefficients */
    float           low, band;      /* SVF state */

    uint32_t        ctl;            /* samples since VoiceOn (control-rate phase) */
    float           gTar[3];        /* L, R, FX targets */
    float           gCur[3];
    double          stepCur;
};

struct DSoftDevice {
    int used;
};

static struct DSoftDevice g_device;
static struct DSoftBuffer g_buffers[DSOFT_MAX_BUFFERS];

/* Build with -DDSOFT_TRACE to log voice commands to stderr (debug aid);
   DSOFT_SOLO=<mask> in the environment then mutes the other voices. */
#ifdef DSOFT_TRACE
#include <stdio.h>
#include <stdlib.h>
static unsigned long g_trace_frames;
#define TRACE(b, ...) do { fprintf(stderr, "%8.3f v%-2d ", (double)g_trace_frames / DSOFT_FS, (int)((b) - g_buffers)); \
                           fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define TRACE(b, ...) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int bit_index(DWORD m)
{
    int i = 0;
    while (i < 32 && !(m & (1u << i)))
        i++;
    return i;
}

static float mb_gain(struct DSoftBuffer *b, DWORD bin)
{
    long att;
    long reg;

    if (!(b->mixmask & bin))
        return 0.0f;
    /* ConvertVolumeValues(): start with -volume, subtract mixbin volume */
    att = -(long)b->volume - (long)b->binvol[bit_index(bin)];
    if (att < 0)
        att = 0;
    reg = (att << 6) / 100;             /* u6.6 dB register */
    if (reg >= 0xFFF)
        return 0.0f;                    /* 0xFFF == mute */
    return (float)pow(10.0, -((double)reg / 64.0) / 20.0);
}

static void update_gains(struct DSoftBuffer *b)
{
    b->gTar[0] = mb_gain(b, DSMIXBIN_FRONT_LEFT);
    b->gTar[1] = mb_gain(b, DSMIXBIN_FRONT_RIGHT);
    b->gTar[2] = mb_gain(b, DSMIXBIN_FXSEND_0);
}

static float sustain_level(const SoftEG *e)
{
    return (float)(e->d.dwSustain & 0xFF) / 256.0f;    /* u.8 */
}

/* 12-bit register fields */
#define EG_LEN(x) (((uint32_t)(x) & 0xFFFu) * DSOFT_EG_UNIT)

static void eg_enter(SoftEG *e, int seg)
{
    for (;;) {
        e->seg = seg;
        switch (seg) {
        case EG_DELAY:
            e->value = 0.0f;
            e->count = EG_LEN(e->d.dwDelay);
            if (e->count)
                return;
            seg = EG_ATTACK;
            break;
        case EG_ATTACK:
            e->count = EG_LEN(e->d.dwAttack);
            if (e->count) {
                e->step = (1.0f - e->value) / (float)e->count;
                return;
            }
            seg = EG_HOLD;
            break;
        case EG_HOLD:
            e->value = 1.0f;
            e->count = EG_LEN(e->d.dwHold);
            if (e->count)
                return;
            seg = EG_DECAY;
            break;
        case EG_DECAY:
            e->target = sustain_level(e);
            e->count = EG_LEN(e->d.dwDecay);
            if (e->count) {
                e->coef = (float)exp(-5.0 / (double)e->count);
                return;
            }
            seg = EG_SUSTAIN;
            break;
        case EG_SUSTAIN:
            e->value = sustain_level(e);
            e->count = 0;
            return;
        case EG_RELEASE:
            e->count = EG_LEN(e->d.dwRelease);
            if (e->count) {
                e->coef = (float)exp(-5.0 / (double)e->count);
                return;
            }
            seg = EG_OFF;
            break;
        default:
            e->seg = EG_OFF;
            e->value = 0.0f;
            e->count = 0;
            return;
        }
    }
}

static void eg_start(SoftEG *e)
{
    switch (e->d.dwMode) {
    case DSEG_MODE_DELAY:   eg_enter(e, EG_DELAY);  break;
    case DSEG_MODE_ATTACK:  e->value = 0.0f; eg_enter(e, EG_ATTACK); break;
    case DSEG_MODE_HOLD:    eg_enter(e, EG_HOLD);   break;
    default:                /* DSEG_MODE_DISABLE: always full scale */
        e->seg = EG_SUSTAIN;
        e->value = 1.0f;
        e->count = 0;
        break;
    }
}

static void eg_release(SoftEG *e)
{
    if (e->seg != EG_OFF)
        eg_enter(e, EG_RELEASE);
}

static float eg_tick(SoftEG *e)
{
    float v = e->value;

    switch (e->seg) {
    case EG_DELAY:
        if (--e->count == 0)
            eg_enter(e, EG_ATTACK);
        break;
    case EG_ATTACK:
        e->value += e->step;
        if (--e->count == 0)
            eg_enter(e, EG_HOLD);
        break;
    case EG_HOLD:
        if (--e->count == 0)
            eg_enter(e, EG_DECAY);
        break;
    case EG_DECAY:
        e->value = e->target + (e->value - e->target) * e->coef;
        if (--e->count == 0)
            eg_enter(e, EG_SUSTAIN);
        break;
    case EG_RELEASE:
        e->value *= e->coef;
        if (--e->count == 0)
            eg_enter(e, EG_OFF);
        break;
    default:
        break;
    }
    return v;
}

static void default_eg(SoftEG *e, DWORD which)
{
    memset(e, 0, sizeof(*e));
    e->d.dwEG = which;
    e->d.dwMode = DSEG_MODE_DISABLE;
    e->d.dwSustain = 0xFF;
    e->seg = EG_SUSTAIN;
    e->value = 1.0f;
}

static int8_t s8field(LONG v)
{
    uint8_t u = (uint8_t)((uint32_t)v & 0xFFu);   /* 8-bit register field */
    return (int8_t)(u >= 0x80 ? (int)u - 256 : (int)u);
}

static float fc_signed(DWORD fc)
{
    uint16_t u = (uint16_t)(fc & 0xFFFFu);          /* 16-bit register field */
    return (float)(u >= 0x8000 ? (long)u - 65536L : (long)u);
}

/* ------------------------------------------------------------------ */
/* device / buffer API                                                 */
/* ------------------------------------------------------------------ */

HRESULT dsoft_Create(void *guid, LPDIRECTSOUND8 *ppDS, void *outer)
{
    (void)guid;
    (void)outer;
    g_device.used = 1;
    dsoft_ResetMixer();
    if (ppDS)
        *ppDS = &g_device;
    return DS_OK;
}

HRESULT dsoft_ReleaseDevice(LPDIRECTSOUND8 pDS)
{
    int i;
    if (pDS != &g_device)
        return E_FAIL;
    for (i = 0; i < DSOFT_MAX_BUFFERS; i++)
        g_buffers[i].used = 0;
    g_device.used = 0;
    return DS_OK;
}

HRESULT dsoft_CreateBuffer(const DSBUFFERDESC *desc, LPDIRECTSOUNDBUFFER8 *ppBuf)
{
    int i;
    struct DSoftBuffer *b = NULL;

    if (!desc || !ppBuf)
        return E_FAIL;
    if (desc->lpwfxFormat &&
        (desc->lpwfxFormat->wBitsPerSample != 16 || desc->lpwfxFormat->nChannels != 1))
        return E_FAIL;  /* only what dev.c uses */

    for (i = 0; i < DSOFT_MAX_BUFFERS; i++) {
        if (!g_buffers[i].used) {
            b = &g_buffers[i];
            break;
        }
    }
    if (!b)
        return E_FAIL;

    memset(b, 0, sizeof(*b));
    b->used = 1;
    b->mixmask = desc->dwMixBinMask;
    b->volume = DSBVOLUME_MAX - DSBHEADROOM_DEFAULT_2D;
    b->pitch = 0;   /* 48 kHz buffer at 48 kHz */
    default_eg(&b->ampEG, DSEG_AMPLITUDE);
    default_eg(&b->multiEG, DSEG_MULTI);
    b->fmode = DSFILTER_MODE_BYPASS;
    update_gains(b);
    memcpy(b->gCur, b->gTar, sizeof(b->gCur));
    *ppBuf = b;
    return DS_OK;
}

HRESULT dsoft_Release(LPDIRECTSOUNDBUFFER8 b)
{
    if (!b)
        return E_FAIL;
    b->used = 0;
    b->active = 0;
    return DS_OK;
}

HRESULT dsoft_SetBufferData(LPDIRECTSOUNDBUFFER8 b, const void *data, DWORD bytes)
{
    if (!b)
        return E_FAIL;
    if ((const int16_t *)data != b->data || bytes / 2 != b->nsamp) {
        /* ReleaseBufferData(): the voice cannot keep playing old data */
        b->active = 0;
        b->cached = 0;
        b->data = (const int16_t *)data;
        b->nsamp = bytes / 2;
        b->loopStart = 0;
        b->loopLen = b->nsamp;
    }
    return DS_OK;
}

HRESULT dsoft_SetLoopRegion(LPDIRECTSOUNDBUFFER8 b, DWORD startBytes, DWORD lenBytes)
{
    uint32_t start, len;
    if (!b)
        return E_FAIL;
    start = startBytes / 2;
    len = lenBytes / 2;
    if (len == 0)
        len = b->nsamp > start ? b->nsamp - start : 0;
    if (start + len > b->nsamp)
        return E_FAIL;  /* "Loop region extends past the end of the play region" */
    b->loopStart = start;
    b->loopLen = len;
    return DS_OK;
}

HRESULT dsoft_SetCurrentPosition(LPDIRECTSOUNDBUFFER8 b, DWORD posBytes)
{
    uint32_t pos;
    if (!b)
        return E_FAIL;
    pos = posBytes / 2;
    if (b->active) {
        if (b->looping && pos >= b->loopStart + b->loopLen)
            b->looping = 0;
        b->pos = (double)pos;
    } else {
        b->cached = pos;
    }
    return DS_OK;
}

HRESULT dsoft_Play(LPDIRECTSOUNDBUFFER8 b, DWORD r1, DWORD r2, DWORD flags)
{
    (void)r1;
    (void)r2;
    if (!b || !b->data || b->nsamp < 2)
        return E_FAIL;

    b->looping = (flags & DSBPLAY_LOOPING) ? 1 : 0;

    if (b->active && !(flags & DSBPLAY_FROMSTART)) {
        TRACE(b, "play (already active%s)", b->noteoff ? ", releasing" : "");
        return DS_OK;
    }
    TRACE(b, "play %s pitch %d", b->looping ? "loop" : "once", (int)b->pitch);   /* ActivateVoice() is a no-op on an active voice */

    b->pos = (flags & DSBPLAY_FROMSTART) ? 0.0 : (double)b->cached;
    b->cached = 0;
    b->active = 1;
    b->noteoff = 0;
    b->ctl = 0;
    eg_start(&b->ampEG);
    eg_start(&b->multiEG);
    /* fresh hardware voice: clear filter memory, snap tracked parameters */
    b->low = b->band = 0.0f;
    b->fcCur = fc_signed(b->fc0);
    b->qCur = (float)(b->fc1 & 0xFFFFu) / 32768.0f;
    update_gains(b);
    memcpy(b->gCur, b->gTar, sizeof(b->gCur));
    return DS_OK;
}

HRESULT dsoft_StopEx(LPDIRECTSOUNDBUFFER8 b, int64_t rtTimeStamp, DWORD flags)
{
    (void)rtTimeStamp;
    if (!b)
        return E_FAIL;
    if (!b->active)
        return DS_OK;
    if (flags & DSBSTOPEX_ENVELOPE) {
        if (flags & DSBSTOPEX_RELEASEWAVEFORM)
            b->looping = 0;
        if (!b->noteoff) {
            TRACE(b, "release");
            b->noteoff = 1;
            eg_release(&b->ampEG);
            eg_release(&b->multiEG);
            if (b->ampEG.seg == EG_OFF)
                b->active = 0;
        }
    } else {
        b->cached = (uint32_t)b->pos;
        b->active = 0;
    }
    return DS_OK;
}

HRESULT dsoft_SetPitch(LPDIRECTSOUNDBUFFER8 b, LONG pitch)
{
    if (!b)
        return E_FAIL;
    b->pitch = pitch;
    TRACE(b, "pitch %d", (int)pitch);
    return DS_OK;
}

HRESULT dsoft_SetVolume(LPDIRECTSOUNDBUFFER8 b, LONG volume)
{
    if (!b)
        return E_FAIL;
    b->volume = volume - DSBHEADROOM_DEFAULT_2D;
    update_gains(b);
    TRACE(b, "volume %d -> gains %.3f %.3f %.3f", (int)volume, b->gTar[0], b->gTar[1], b->gTar[2]);
    return DS_OK;
}

HRESULT dsoft_SetMixBinVolumes(LPDIRECTSOUNDBUFFER8 b, DWORD mask, const LONG *vols)
{
    int n = 0;
    if (!b || !vols)
        return E_FAIL;
    /* SetMixBinVolumes_v1: volumes are given in lsb-first order of the mask */
    while (mask) {
        int bin = bit_index(mask);
        b->binvol[bin] = vols[n++];
        mask &= ~(1u << bin);
    }
    update_gains(b);
    return DS_OK;
}

HRESULT dsoft_SetEG(LPDIRECTSOUNDBUFFER8 b, LPCDSENVELOPEDESC env)
{
    SoftEG *e;
    if (!b || !env)
        return E_FAIL;
    if (env->dwEG == DSEG_AMPLITUDE)
        e = &b->ampEG;
    else if (env->dwEG == DSEG_MULTI)
        e = &b->multiEG;
    else
        return E_FAIL;
    /* registers change; the running segment keeps going (new lengths are
       picked up at the next segment transition / VoiceOn) */
    e->d = *env;
    if (!b->active) {
        e->seg = EG_OFF;
        e->value = 0.0f;
    }
    return DS_OK;
}

HRESULT dsoft_SetFilter(LPDIRECTSOUNDBUFFER8 b, LPCDSFILTERDESC f)
{
    if (!b || !f)
        return E_FAIL;
    if (b->fmode == DSFILTER_MODE_BYPASS && f->dwMode != DSFILTER_MODE_BYPASS) {
        /* filter switched on: start tracking from the new values */
        b->fcCur = fc_signed(f->adwCoefficients[0]);
        b->qCur = (float)(f->adwCoefficients[1] & 0xFFFFu) / 32768.0f;
    }
    b->fmode = f->dwMode & 3u;
    b->fc0 = f->adwCoefficients[0] & 0xFFFFu;   /* MCPX_MAKE_REG_VALUE masks */
    b->fc1 = f->adwCoefficients[1] & 0xFFFFu;
    TRACE(b, "filter fc0 %u (%.0f Hz) fc1 %u", (unsigned)b->fc0,
          48000.0 / 3.14159265 * asin(0.5 * (fc_signed(b->fc0) > 0 ? 1.0 : pow(2.0, fc_signed(b->fc0) / 4096.0))), (unsigned)b->fc1);
    return DS_OK;
}

/* ------------------------------------------------------------------ */
/* reverb approximation (boot DSP image: I3DL2 "Bathroom")             */
/* ------------------------------------------------------------------ */

#define RV_LINES     4
#define RV_MAXLEN    2048
#define RV_PRE_LEN   1024

static const int   rv_len[RV_LINES] = { 613, 761, 929, 1109 };
static float       rv_buf[RV_LINES][RV_MAXLEN];
static int         rv_idx[RV_LINES];
static float       rv_lp[RV_LINES];
static float       rv_gain[RV_LINES];
static float       rv_pre[RV_PRE_LEN];
static int         rv_pre_idx;
static float       dc_x[2], dc_y[2];
static int         rv_init;

#define RV_RT60          1.49f
#define RV_DAMP          0.35f      /* HF decay ratio 0.54 */
#define RV_REFL_DELAY    336        /* 7 ms  */
#define RV_LATE_DELAY    (336 + 528)/* +11 ms */
#define RV_ROOM          0.316f     /* Room -1000 mB */
#define RV_REFL_GAIN     0.66f      /* Reflections -370 mB */
#define RV_LATE_GAIN     0.42f      /* Reverb +1030 mB, FDN-normalised */

void dsoft_ResetMixer(void)
{
    int i;
    memset(rv_buf, 0, sizeof(rv_buf));
    memset(rv_idx, 0, sizeof(rv_idx));
    memset(rv_lp, 0, sizeof(rv_lp));
    memset(rv_pre, 0, sizeof(rv_pre));
    rv_pre_idx = 0;
    dc_x[0] = dc_x[1] = dc_y[0] = dc_y[1] = 0.0f;
    for (i = 0; i < RV_LINES; i++)
        rv_gain[i] = (float)pow(10.0, -3.0 * (double)rv_len[i] / (RV_RT60 * DSOFT_FS));
    rv_init = 1;
}

static void reverb_tick(float in, float *outL, float *outR)
{
    float o[RV_LINES];
    float sum = 0.0f, refl, late;
    int i;

    rv_pre[rv_pre_idx] = in * RV_ROOM;
    refl = rv_pre[(rv_pre_idx - RV_REFL_DELAY + RV_PRE_LEN) % RV_PRE_LEN];
    late = rv_pre[(rv_pre_idx - RV_LATE_DELAY + RV_PRE_LEN) % RV_PRE_LEN];
    rv_pre_idx = (rv_pre_idx + 1) % RV_PRE_LEN;

    for (i = 0; i < RV_LINES; i++) {
        o[i] = rv_buf[i][rv_idx[i]];
        sum += o[i];
    }
    sum *= 0.5f;    /* Householder: o - 2/N * sum */
    for (i = 0; i < RV_LINES; i++) {
        float fb = (o[i] - sum) * rv_gain[i];
        rv_lp[i] += (fb - rv_lp[i]) * (1.0f - RV_DAMP);
        rv_buf[i][rv_idx[i]] = late + rv_lp[i];
        if (++rv_idx[i] >= rv_len[i])
            rv_idx[i] = 0;
    }
    *outL = refl * RV_REFL_GAIN + (o[0] + o[2]) * RV_LATE_GAIN;
    *outR = refl * RV_REFL_GAIN * 0.8f + (o[1] - o[3]) * RV_LATE_GAIN;
}

/* ------------------------------------------------------------------ */
/* voice rendering                                                     */
/* ------------------------------------------------------------------ */

static void voice_frame_params(struct DSoftBuffer *b)
{
    float envF = b->multiEG.value;
    float p, fc, qt;

    /* pitch: s3.12 octaves + multi EG * pitch scale (s.7 octaves) */
    p = (float)b->pitch + (float)s8field(b->multiEG.d.lPitchScale) * 32.0f * envF;
    if (p < -32768.0f) p = -32768.0f;
    if (p > 8191.0f)   p = 8191.0f;
    b->stepCur = pow(2.0, (double)p / 4096.0);

    if (b->fmode != DSFILTER_MODE_BYPASS) {
        /* track the target cutoff (in log domain) and damping */
        b->fcCur += (fc_signed(b->fc0) - b->fcCur) * 0.25f;
        qt = (float)b->fc1 / 32768.0f;
        b->qCur += (qt - b->qCur) * 0.25f;

        /* multi EG * FC scale (s3.4 octaves) => 4096/16 = 256 per LSB */
        fc = b->fcCur + (float)s8field(b->multiEG.d.lFilterCutOff) * 256.0f * envF;
        if (fc > 0.0f)       fc = 0.0f;         /* f <= 1 : ~8 kHz, stable */
        if (fc < -32768.0f)  fc = -32768.0f;
        b->f = (float)pow(2.0, (double)fc / 4096.0);
        b->q = b->qCur;
        if (b->q < 0.02f) b->q = 0.02f;
        if (b->q > 2.0f)  b->q = 2.0f;
    }
}

static void render_voice(struct DSoftBuffer *b, float *out, float *fx, int frames)
{
    int n;
    const float ga = 1.0f / 128.0f;     /* ~2.7 ms volume tracking */

    for (n = 0; n < frames; n++) {
        uint32_t i0, i1, end;
        float frac, s, amp;

        if (!b->active)
            break;

        if ((b->ctl++ & (DSOFT_FRAME - 1)) == 0)
            voice_frame_params(b);

        end = b->looping ? b->loopStart + b->loopLen : b->nsamp;
        i0 = (uint32_t)b->pos;
        if (i0 >= end) {    /* can only happen if region shrank */
            if (b->looping && b->loopLen) {
                b->pos = b->loopStart;
                i0 = b->loopStart;
            } else {
                b->active = 0;
                break;
            }
        }
        frac = (float)(b->pos - (double)i0);
        i1 = i0 + 1;
        s = (float)b->data[i0];
        if (i1 >= end) {
            /* wrap to loop start, or interpolate towards silence past EBO */
            float s1 = b->looping ? (float)b->data[b->loopStart] : 0.0f;
            s += (s1 - s) * frac;
        } else {
            s += ((float)b->data[i1] - s) * frac;
        }

        if (b->fmode != DSFILTER_MODE_BYPASS) {
            /* Chamberlin state variable filter, low-pass output */
            float high;
            b->low += b->f * b->band;
            high = s - b->low - b->q * b->band;
            b->band += b->f * high;
            s = b->low;
        }

        amp = eg_tick(&b->ampEG);
        (void)eg_tick(&b->multiEG);
        s *= amp;

        b->gCur[0] += (b->gTar[0] - b->gCur[0]) * ga;
        b->gCur[1] += (b->gTar[1] - b->gCur[1]) * ga;
        b->gCur[2] += (b->gTar[2] - b->gCur[2]) * ga;
        out[2 * n]     += s * b->gCur[0];
        out[2 * n + 1] += s * b->gCur[1];
        fx[n]          += s * b->gCur[2];

        if (b->noteoff && b->ampEG.seg == EG_OFF) {
            b->active = 0;      /* end of release: voice off */
            break;
        }

        b->pos += b->stepCur;
        if (b->looping) {
            double le = (double)b->loopStart + (double)b->loopLen;
            if (b->loopLen == 0) {
                b->active = 0;
                break;
            }
            while (b->pos >= le)
                b->pos -= (double)b->loopLen;
        } else if (b->pos >= (double)b->nsamp) {
            b->active = 0;      /* end of one-shot buffer: voice off */
            break;
        }
    }
}

void dsoft_Render(float *out, int frames)
{
    enum { CHUNK = 256 };
    float fx[CHUNK];
    float mix[2 * CHUNK];
    const float headroom = 1.0f / (float)(1 << DSHEADROOM_DEFAULT);
    const float R = 1.0f - (2.0f * 3.14159265f * 10.0f / (float)DSOFT_FS);
    int done = 0;

    if (!rv_init)
        dsoft_ResetMixer();

    while (done < frames) {
        int n = frames - done;
        int i, k;
        if (n > CHUNK)
            n = CHUNK;
        memset(fx, 0, sizeof(float) * (size_t)n);
        memset(mix, 0, sizeof(float) * 2 * (size_t)n);

        if (g_device.used) {
#ifdef DSOFT_TRACE
            const char *solo = getenv("DSOFT_SOLO");
            unsigned long mask = solo ? strtoul(solo, NULL, 0) : ~0ul;
#endif
            for (i = 0; i < DSOFT_MAX_BUFFERS; i++) {
                struct DSoftBuffer *b = &g_buffers[i];
                if (b->used && b->active && b->data) {
#ifdef DSOFT_TRACE
                    if (!(mask & (1ul << i))) {
                        float dm[2 * CHUNK], df[CHUNK];
                        render_voice(b, dm, df, n);
                        continue;
                    }
#endif
                    render_voice(b, mix, fx, n);
#ifdef DSOFT_TRACE
                    if (!b->active)
                        TRACE(b, "voice off");
#endif
                }
            }
        }
#ifdef DSOFT_TRACE
        g_trace_frames += (unsigned long)n;
#endif

        for (k = 0; k < n; k++) {
            float rl, rr, l, r;
            reverb_tick(fx[k], &rl, &rr);
            l = (mix[2 * k] + rl) * headroom;
            r = (mix[2 * k + 1] + rr) * headroom;
            /* AC-coupled output */
            dc_y[0] = l - dc_x[0] + R * dc_y[0];
            dc_x[0] = l;
            dc_y[1] = r - dc_x[1] + R * dc_y[1];
            dc_x[1] = r;
            out[2 * (done + k)]     = dc_y[0];
            out[2 * (done + k) + 1] = dc_y[1];
        }
        done += n;
    }
}
