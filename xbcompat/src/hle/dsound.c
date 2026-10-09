/*
 * DirectSound (dsound.lib / dsoundd.lib) replaced by a software mixer on SDL2.
 *
 * The Xbox library is a thin C API (DirectSound*, IDirectSound*_*, XAudio*,
 * XWaveFile*, XFileCreate*) over C++ classes (CDirectSound, CDirectSoundBuffer,
 * CDirectSoundStream, CDirectSoundVoice) that program the MCPX audio
 * processor.  Both layers are replaced here: the C exports and the public C++
 * methods, so that a C thunk findsigs could not locate (the thunks are
 * byte-identical except for one rel32) still lands in our code when it calls
 * its C++ method.
 *
 * Object pointers.  IDirectSound and IDirectSoundBuffer are opaque; the pointer
 * the title holds (p) is an empty base placed after the polymorphic bases, so
 * the C++ methods receive this = p - K with a build-specific K (CValidObject
 * adds 8 bytes in dsoundd.lib).  Verified from the library bytes:
 *   CDirectSound        this = p-0x10 (debug) / p-8 (release), CRefCount at p-8
 *   CDirectSoundBuffer  this = p-0x24 / p-0x1C, CDirectSoundVoice (= CRefCount) at p-0x1C
 *   CDirectSoundStream  this = p (it starts with the XMediaObject vtable),
 *                       CDirectSoundVoice at p+0xC / p+4
 * Every entry point resolves the pointer it receives through a registry of
 * live objects that knows all of these aliases, so one implementation serves
 * the C thunk, the C++ method and the voice-level method alike.  A fake
 * CRefCount header {vtable, refs} sits at the offset the library expects, with
 * our AddRef/Release in its vtable slots, so even an unpatched AddRef/Release
 * thunk behaves.
 *
 * Mixing.  All voices are rendered to 48 kHz stereo float in the SDL audio
 * callback (or by a silent clock thread when no audio device opens), resampled
 * by pitch (4096 units per octave, as the hardware) with linear interpolation,
 * Xbox ADPCM decoded on the fly, volume in hundredths of dB, mixbins folded to
 * stereo, and the Pan3D distance/cone/pan model for 3D voices.  Stream packet
 * completions are queued by the mixer and delivered on the title's thread from
 * DirectSoundDoWork or the next call on that stream (the library does the same
 * without DSSTREAMCAPS_ACCURATENOTIFY); ACCURATENOTIFY streams are served by a
 * host thread adopted as a guest thread.
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../xbcompat.h"
#include "hle.h"
#include "../cpu.h"

typedef int32_t HRESULT;

#define DS_OK                0
#define DSERR_CONTROLUNAVAIL ((HRESULT)0x8878001E)
#define DSERR_INVALIDCALL    ((HRESULT)0x88780032)
#define DSERR_GENERIC        ((HRESULT)0x80004005)
#define DSERR_OUTOFMEMORY    ((HRESULT)0x8007000E)
#define DSERR_UNSUPPORTED    ((HRESULT)0x80004001)
#define DSERR_INVALIDPARAM   ((HRESULT)0x80070057)

#define XMEDIAPACKET_STATUS_SUCCESS 0u
#define XMEDIAPACKET_STATUS_PENDING 0x8000000Au
#define XMEDIAPACKET_STATUS_FLUSHED 0x80004004u
#define XMEDIAPACKET_STATUS_FAILURE 0x80004005u

#define XMO_STREAMF_FIXED_SAMPLE_SIZE 1
#define XMO_STREAMF_INPUT_ASYNC       4
#define XMO_STATUSF_ACCEPT_INPUT_DATA  1
#define XMO_STATUSF_ACCEPT_OUTPUT_DATA 2

#define WAVE_FORMAT_PCM        1
#define WAVE_FORMAT_XBOX_ADPCM 0x69
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

#define DSBCAPS_CTRL3D             0x10
#define DSBCAPS_CTRLFREQUENCY      0x20
#define DSBCAPS_CTRLVOLUME         0x80
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x100
#define DSBCAPS_MIXIN              0x2000
#define DSBCAPS_LOCDEFER           0x40000
#define DSBCAPS_FXIN               0x80000
#define DSBCAPS_VALID              0xC21B0
#define DSBCAPS_SUBMIXMASK         (DSBCAPS_MIXIN | DSBCAPS_FXIN)
#define DSSTREAMCAPS_ACCURATENOTIFY 0x40000000
#define DSSTREAMCAPS_VALID         0x400400B0

#define DSBPLAY_LOOPING   1
#define DSBPLAY_FROMSTART 2
#define DSBPLAY_SYNCHPLAYBACK      0x4   /* XDK 5xxx: start with CDirectSound::SynchPlayback; played at once here */
#define DSBSTOPEX_ENVELOPE        1
#define DSBSTOPEX_RELEASEWAVEFORM 2
#define DSBSTATUS_PLAYING 1
#define DSBSTATUS_LOOPING 4
#define DSBSTATUS_PAUSED 2
#define DSBLOCK_FROMWRITECURSOR 1
#define DSBLOCK_ENTIREBUFFER    2
#define DSBPN_OFFSETSTOP 0xFFFFFFFFu

#define DSSTREAMSTATUS_READY   1
#define DSSTREAMSTATUS_PLAYING 0x10000
#define DSSTREAMSTATUS_PAUSED  0x20000
#define DSSTREAMSTATUS_STARVED 0x40000
#define DSSTREAMFLUSHEX_ASYNC    1
#define DSSTREAMFLUSHEX_ENVELOPE 2

#define DS3D_DEFERRED 1
#define DS3DMODE_HEADRELATIVE 1
#define DS3DMODE_DISABLE      2

#define DSBVOLUME_MIN (-10000)
#define DSBPITCH_MIN  (-32767)
#define DSBPITCH_MAX  8191
#define DSBFREQUENCY_MIN 188
#define DSBFREQUENCY_MAX 191983
#define DSBHEADROOM_MAX 10000
#define DSBHEADROOM_DEFAULT_2D 600
#define DSBSIZE_MAX 0x0FFFFFFF

#define DSMIXBIN_COUNT 32
#define DSMIXBIN_ASSIGNMENT_MAX 8
#define DSMIXBIN_SUBMIX 31

#define MIX_RATE   48000
#define MIX_FRAMES 512     /* frames per mixer block */
#define HW_FRAME   32      /* hardware frame, samples */

/* ---- Xbox structures (i386 layout, WAVEFORMATEX family is packed) --------- */

typedef struct __attribute__((packed)) {
    uint16_t wFormatTag, nChannels;
    uint32_t nSamplesPerSec, nAvgBytesPerSec;
    uint16_t nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX;

typedef struct __attribute__((packed)) {
    WAVEFORMATEX wfx;
    uint16_t wSamplesPerBlock;
} XBOXADPCMWAVEFORMAT;

typedef struct __attribute__((packed)) {
    WAVEFORMATEX Format;
    uint16_t wValidBitsPerSample;
    uint32_t dwChannelMask;
    uint8_t SubFormat[16];
} WAVEFORMATEXTENSIBLE;

typedef struct {
    void *pvBuffer;
    DWORD dwMaxSize;
    DWORD *pdwCompletedSize;
    DWORD *pdwStatus;
    void *pContext;          /* or HANDLE hCompletionEvent */
    int64_t *prtTimestamp;
} XMEDIAPACKET;

typedef struct { DWORD dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead; } XMEDIAINFO;
typedef struct { DWORD dwFree2DBuffers, dwFree3DBuffers, dwFreeBufferSGEs, dwMemoryAllocated; } DSCAPS;
typedef struct { DWORD dwMixBin; LONG lVolume; } DSMIXBINVOLUMEPAIR;
typedef struct { DWORD dwMixBinCount; const DSMIXBINVOLUMEPAIR *lpMixBinVolumePairs; } DSMIXBINS;
typedef struct {
    DWORD dwSize, dwFlags, dwBufferBytes;
    WAVEFORMATEX *lpwfxFormat;
    const DSMIXBINS *lpMixBins;
    DWORD dwInputMixBin;
} DSBUFFERDESC;
typedef struct { DWORD dwOffset; HANDLE hEventNotify; } DSBPOSITIONNOTIFY;
typedef void (NTAPI *LPFNXMEDIAOBJECTCALLBACK)(void *, void *, DWORD);
typedef struct {
    DWORD dwFlags, dwMaxAttachedPackets;
    WAVEFORMATEX *lpwfxFormat;
    LPFNXMEDIAOBJECTCALLBACK lpfnCallback;
    void *lpvContext;
    const DSMIXBINS *lpMixBins;
} DSSTREAMDESC;
typedef struct {
    DWORD dwSize;
    float vPosition[3], vVelocity[3];
    DWORD dwInsideConeAngle, dwOutsideConeAngle;
    float vConeOrientation[3];
    LONG lConeOutsideVolume;
    float flMinDistance, flMaxDistance;
    DWORD dwMode;
    float flDistanceFactor, flRolloffFactor, flDopplerFactor;
} DS3DBUFFER;
typedef struct {
    DWORD dwSize;
    float vPosition[3], vVelocity[3], vOrientFront[3], vOrientTop[3];
    float flDistanceFactor, flRolloffFactor, flDopplerFactor;
} DS3DLISTENER;
typedef struct { DWORD dwI3DL2ReverbIndex, dwCrosstalkIndex; } DSEFFECTIMAGELOC;
typedef struct {
    void *lpvCodeSegment; DWORD dwCodeSize;
    void *lpvStateSegment; DWORD dwStateSize;
    void *lpvYMemorySegment; DWORD dwYMemorySize;
    void *lpvScratchSegment; DWORD dwScratchSize;
} DSEFFECTMAP;
typedef struct { DWORD dwEffectCount, dwTotalScratchSize; DSEFFECTMAP aEffectMaps[1]; } DSEFFECTIMAGEDESC;

_Static_assert(sizeof(WAVEFORMATEX) == 18 && sizeof(XBOXADPCMWAVEFORMAT) == 20 &&
               sizeof(WAVEFORMATEXTENSIBLE) == 40, "wave formats");
_Static_assert(sizeof(XMEDIAPACKET) == 24 && sizeof(DSBUFFERDESC) == 24 && sizeof(DSSTREAMDESC) == 24 &&
               sizeof(DS3DBUFFER) == 76 && sizeof(DS3DLISTENER) == 64 && sizeof(DSEFFECTMAP) == 32, "ds structs");

/* ---- object model --------------------------------------------------------- */

struct ds_refhdr { void **vtbl; uint32_t refs; };   /* fake CRefCount */

enum ds_kind { DS_DSOUND = 1, DS_BUFFER, DS_STREAM };

/* Sample format as the mixer needs it.  ADPCM is addressed in 64-frame blocks
   of 36*ch bytes; everything else in frames of nBlockAlign bytes. */
struct ds_fmt {
    uint16_t tag, ch, bits, align;
    uint32_t rate;
    uint32_t unit_bytes, unit_frames;   /* bytes <-> frames conversion unit */
    int adpcm;
};

struct ds_3d {
    float pos[3], vel[3];
    DWORD cone_in, cone_out;
    float cone_dir[3];
    LONG cone_vol;
    float min_dist, max_dist;
    DWORD mode;
    float dist_factor, rolloff, doppler;
};

struct ds_listener {
    float pos[3], vel[3], front[3], top[3];
    float dist_factor, rolloff, doppler;
};

struct ds_voice {
    int kind;                 /* DS_BUFFER or DS_STREAM */
    void *owner;              /* the ds_buffer / ds_stream */
    DWORD flags;
    struct ds_fmt fmt;
    LONG pitch;               /* user pitch (SetPitch / SetFrequency), 4096 per octave */
    LONG volume;              /* m_lVolume = user volume - headroom */
    DWORD headroom;
    DWORD nbins; uint8_t bins[DSMIXBIN_ASSIGNMENT_MAX]; LONG binvol[DSMIXBIN_COUNT];
    struct ds_buffer *output; /* SetOutputBuffer target; holds a reference on it */
    /* 3D */
    struct ds_3d p3d, p3d_def;
    int def_dirty;
    LONG l3d_vol, l3d_doppler;
    LONG pan[DSMIXBIN_COUNT];
    int dirty;                /* recompute 3D + gains before the next block */
    float gain[6][2];         /* per source channel: left, right */
    float subgain[6];         /* per source channel: into the output (submix) buffer */
    /* voice processor: the two envelope generators and the filter */
    struct ds_eg { DWORD d[10]; int seg; uint32_t count; float value, step, coef, target; } eg[2];
    int noteoff;              /* StopEx(ENVELOPE): releasing, off when the amplitude EG ends */
    DWORD fmode, fc0, fc1;    /* SetFilter: mode, DLS2 cutoff and resonance registers */
    float fc_cur, q_cur, svf_f, svf_q, low[6], band[6];
    uint32_t ctl;             /* frames since VoiceOn: control-rate phase */
    double step_cur;
    /* decoded ADPCM block cache (mixer-owned) */
    int16_t adpcm_cache[2 * 64];
    int32_t adpcm_block;
    const uint8_t *adpcm_base;
};

struct ds_notify { DWORD offset; HANDLE event; };

struct ds_buffer {
    uint32_t valid_vptr, valid_sig;   /* p-0x24: debug CValidObject image */
    struct ds_refhdr hdr;             /* p-0x1C: CDirectSoundVoice / CRefCount */
    uint32_t pad[5];                  /* p-0x14 .. p-1 */
    uint8_t iface[8];                 /* p: the empty IDirectSoundBuffer base */
    struct ds_voice v;
    uint8_t *data; DWORD size; int app_owned;
    DWORD play_start, play_len, loop_start, loop_len;   /* bytes; loop relative to play start */
    DWORD cursor;                     /* cached play cursor (bytes from play start) while stopped */
    int playing, looping;
    int paused;                       /* IDirectSoundBuffer_Pause (XDK 5xxx) */
    double pos;                       /* play cursor in frames from play start (mixer) */
    int64_t start_at, stop_at;        /* REFERENCE_TIME deadlines, 0 = none */
    DWORD start_flags, stop_flags;
    struct ds_notify *notify; DWORD nnotify;   /* sorted ascending, OFFSETSTOP last */
    DWORD notify_next, notify_last;   /* next entry to signal, cursor (bytes) of the last evaluation */
    int notify_active;                /* position checks registered (the library's POSITIONDELTA command) */
    DWORD input_mixbin;
    float *sub;                       /* MIXIN/FXIN: this block's submix input (mono, 48 kHz) */
};
_Static_assert(offsetof(struct ds_buffer, iface) == 0x24, "buffer layout");
_Static_assert(offsetof(struct ds_buffer, hdr) == 8, "buffer header");

struct ds_packet { XMEDIAPACKET xmp; DWORD frames; };

struct ds_done {
    XMEDIAPACKET xmp;
    DWORD status;
    LPFNXMEDIAOBJECTCALLBACK cb;
    void *ctx;
    struct ds_stream *s;
};

struct ds_stream {
    void **vtbl;                      /* p: IDirectSoundStream / XMediaObject vtable */
    struct ds_refhdr hdr_rel;         /* p+4: release-build CRefCount */
    struct ds_refhdr hdr_dbg;         /* p+0xC: debug-build CRefCount */
    struct ds_voice v;
    DWORD max_packets;
    struct ds_packet *pk; DWORD head, count;   /* ring of accepted packets */
    LPFNXMEDIAOBJECTCALLBACK callback; void *context;
    int active, paused, starved, discontinuity, flush_pending;
    double pos;                       /* frames into the head packet */
    struct ds_done *done; DWORD ndone, done_cap;   /* completed, not yet reported */
};
_Static_assert(offsetof(struct ds_stream, hdr_rel) == 4 && offsetof(struct ds_stream, hdr_dbg) == 0xC,
               "stream layout");

struct ds_object {
    uint32_t valid_vptr, valid_sig;   /* p-0x10 */
    struct ds_refhdr hdr;             /* p-8 */
    uint8_t iface[8];                 /* p */
    DWORD speaker_config;
    uint8_t mixbin_headroom[DSMIXBIN_COUNT];
    int headphones;
    struct ds_listener listener, listener_def;
    int ldef_dirty;
    DWORD fx_count; DSEFFECTIMAGEDESC *fx_desc; uint8_t **fx_state; DWORD *fx_state_size;
    DSEFFECTIMAGELOC fx_loc;
};
_Static_assert(offsetof(struct ds_object, iface) == 0x10, "dsound layout");

struct ds_entry { int kind; void *obj; uint32_t p; };

static struct {
    pthread_mutex_t lock;
    struct ds_object *ds;             /* the singleton, NULL when not created */
    struct ds_entry *objs; unsigned nobjs, objcap;
    int algorithm;                    /* 0 none, 1 full HRTF, 2 light HRTF, 3 Pan3D */
    DWORD override_speaker;           /* DSSPEAKER_USE_DEFAULT = none */
    /* audio device */
    int audio_mode;                   /* 0 not started, 1 SDL device, 2 clock thread, 3 manual (tests) */
    SDL_AudioDeviceID dev;
    uint64_t frames;                  /* 48 kHz frames rendered */
    uint64_t block_ns;                /* host time of the last block */
    volatile int stopping;
    /* ACCURATENOTIFY completions */
    int cthread_started;
    pthread_cond_t ccond;
    struct ds_done *acc; DWORD nacc, acc_cap;
    DWORD mem_allocated;
    /* statistics, logged at exit */
    float peak; uint64_t audible_frames; unsigned plays, packets_done;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .override_speaker = 0xFFFFFFFFu, .ccond = PTHREAD_COND_INITIALIZER };

/* Tests can drive the mixer by hand: set this before any DirectSound call. */
int g_ds_no_audio_thread;

#define LOCK()   pthread_mutex_lock(&g.lock)
#define UNLOCK() pthread_mutex_unlock(&g.lock)

static void NTAPI hdr_dtor(void *self) { (void)self; }
static ULONG NTAPI Obj_AddRef(void *self);
static ULONG NTAPI Obj_Release(void *self);
static void *refhdr_vtbl[3] = { (void *)hdr_dtor, (void *)Obj_AddRef, (void *)Obj_Release };
static void guest_vtbls(void);

static void registry_add(int kind, void *obj, uint32_t p)
{
    if (g.nobjs == g.objcap) {
        g.objcap = g.objcap ? g.objcap * 2 : 64;
        g.objs = realloc(g.objs, g.objcap * sizeof(*g.objs));
    }
    g.objs[g.nobjs++] = (struct ds_entry){ kind, obj, p };
}

static void registry_remove(void *obj)
{
    for (unsigned i = 0; i < g.nobjs; i++)
        if (g.objs[i].obj == obj) { g.objs[i] = g.objs[--g.nobjs]; return; }
}

/* Resolve an app pointer or any C++ this alias (see the header comment). */
static struct ds_entry *ds_find(const void *self)
{
    uint32_t a = (uint32_t)self;
    if (!a) return NULL;
    for (unsigned i = 0; i < g.nobjs; i++) {
        struct ds_entry *e = &g.objs[i];
        uint32_t p = e->p;
        switch (e->kind) {
        case DS_DSOUND: if (a == p || a == p - 8 || a == p - 0x10) return e; break;
        case DS_BUFFER: if (a == p || a == p - 0x1C || a == p - 0x24) return e; break;
        case DS_STREAM: if (a == p || a == p + 4 || a == p + 0xC) return e; break;
        }
    }
    return NULL;
}

static struct ds_object *find_ds(const void *self)
{
    struct ds_entry *e = ds_find(self);
    return e && e->kind == DS_DSOUND ? e->obj : NULL;
}

static struct ds_buffer *find_buffer(const void *self)
{
    struct ds_entry *e = ds_find(self);
    return e && e->kind == DS_BUFFER ? e->obj : NULL;
}

static struct ds_stream *find_stream(const void *self)
{
    struct ds_entry *e = ds_find(self);
    return e && e->kind == DS_STREAM ? e->obj : NULL;
}

static struct ds_voice *find_voice(const void *self)
{
    struct ds_entry *e = ds_find(self);
    if (!e) return NULL;
    if (e->kind == DS_BUFFER) return &((struct ds_buffer *)e->obj)->v;
    if (e->kind == DS_STREAM) return &((struct ds_stream *)e->obj)->v;
    return NULL;
}

static void mark_all_3d_dirty(void)
{
    for (unsigned i = 0; i < g.nobjs; i++) {
        struct ds_voice *v = NULL;
        if (g.objs[i].kind == DS_BUFFER) v = &((struct ds_buffer *)g.objs[i].obj)->v;
        else if (g.objs[i].kind == DS_STREAM) v = &((struct ds_stream *)g.objs[i].obj)->v;
        if (v && (v->flags & DSBCAPS_CTRL3D)) v->dirty = 1;
    }
}

/* ---- formats --------------------------------------------------------------- */

static const uint8_t ksdataformat_tail[12] = { 0x00, 0x00, 0x00, 0x10, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

static HRESULT fmt_parse(const WAVEFORMATEX *w, struct ds_fmt *f, DWORD *channel_mask)
{
    if (!w) return DSERR_INVALIDPARAM;
    uint16_t tag = w->wFormatTag;
    DWORD mask = 0;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)w;
        if (w->cbSize < 22 || memcmp(x->SubFormat + 4, ksdataformat_tail, 12)) return DSERR_INVALIDPARAM;
        uint32_t sub; memcpy(&sub, x->SubFormat, 4);
        if (sub != WAVE_FORMAT_PCM && sub != WAVE_FORMAT_XBOX_ADPCM) return DSERR_INVALIDPARAM;
        tag = (uint16_t)sub;
        mask = x->dwChannelMask & 0x3F;
    }
    memset(f, 0, sizeof(*f));
    f->tag = tag; f->ch = w->nChannels; f->bits = w->wBitsPerSample; f->align = w->nBlockAlign;
    f->rate = w->nSamplesPerSec;
    if (f->rate < DSBFREQUENCY_MIN || f->rate > DSBFREQUENCY_MAX) return DSERR_INVALIDPARAM;
    if (tag == WAVE_FORMAT_PCM) {
        if (!(f->ch == 1 || f->ch == 2 || f->ch == 4 || f->ch == 6)) return DSERR_INVALIDPARAM;
        if (f->bits != 8 && f->bits != 16) return DSERR_INVALIDPARAM;
        if (f->align != f->ch * f->bits / 8) return DSERR_INVALIDPARAM;
        f->unit_bytes = f->align; f->unit_frames = 1;
    } else if (tag == WAVE_FORMAT_XBOX_ADPCM) {
        if (f->ch != 1 && f->ch != 2) return DSERR_INVALIDPARAM;
        if (f->bits != 4 || f->align != 36 * f->ch) return DSERR_INVALIDPARAM;
        if (w->wFormatTag == WAVE_FORMAT_XBOX_ADPCM &&
            (w->cbSize < 2 || ((const XBOXADPCMWAVEFORMAT *)w)->wSamplesPerBlock != 64))
            return DSERR_INVALIDPARAM;
        f->adpcm = 1; f->unit_bytes = 36 * f->ch; f->unit_frames = 64;
    } else {
        return DSERR_INVALIDPARAM;
    }
    if (channel_mask) *channel_mask = mask;
    return DS_OK;
}

static inline DWORD bytes_to_frames(const struct ds_fmt *f, DWORD bytes)
{
    return bytes / f->unit_bytes * f->unit_frames;
}

static inline DWORD frames_to_bytes(const struct ds_fmt *f, DWORD frames)
{
    return frames / f->unit_frames * f->unit_bytes;
}

/* XAudioCalculatePitch: 4096 * log2(f / 48000), rounded like fistp. */
static LONG calc_pitch(DWORD freq)
{
    if (freq == MIX_RATE) return 0;
    return (LONG)lrint(4096.0 * log2((double)freq / MIX_RATE));
}

/* ---- Xbox ADPCM ------------------------------------------------------------ */

static const int16_t adpcm_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t adpcm_index_adjust[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline int adpcm_sample(int nibble, int *pred, int *idx)
{
    int step = adpcm_step[*idx];
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 8) diff = -diff;
    int s = *pred + diff;
    if (s < -32768) s = -32768; else if (s > 32767) s = 32767;
    *pred = s;
    *idx += adpcm_index_adjust[nibble];
    if (*idx < 0) *idx = 0; else if (*idx > 88) *idx = 88;
    return s;
}

/* Decode one 64-frame block (36 bytes per channel) to interleaved 16-bit PCM.
   Returns 0 and silence for a corrupt step index, as the library does. */
int ds_adpcm_decode_block(const uint8_t *src, int ch, int16_t *out)
{
    int pred[2], idx[2];
    for (int c = 0; c < ch; c++) {
        uint32_t h; memcpy(&h, src + 4 * c, 4);
        pred[c] = (int16_t)(h & 0xFFFF);
        idx[c] = (h >> 16) & 0xFF;
        if (idx[c] > 88) { memset(out, 0, 64 * ch * sizeof(*out)); return 0; }
        out[c] = (int16_t)pred[c];
    }
    const uint8_t *p = src + 4 * ch;
    if (ch == 1) {
        for (int k = 1; k < 64; k++) {
            int b = p[(k - 1) >> 1];
            int n = (k & 1) ? b & 0xF : b >> 4;
            out[k] = (int16_t)adpcm_sample(n, &pred[0], &idx[0]);
        }
    } else {
        int k = 1;
        for (int grp = 0; grp < 8; grp++) {
            uint32_t l, r;
            memcpy(&l, p + grp * 8, 4);
            memcpy(&r, p + grp * 8 + 4, 4);
            for (int i = 0; i < 8 && k < 64; i++, k++) {
                out[k * 2] = (int16_t)adpcm_sample(l & 0xF, &pred[0], &idx[0]);
                out[k * 2 + 1] = (int16_t)adpcm_sample(r & 0xF, &pred[1], &idx[1]);
                l >>= 4; r >>= 4;
            }
        }
    }
    return 1;
}

/* ---- sample fetch ---------------------------------------------------------- */

struct ds_src {
    const uint8_t *data;
    DWORD nframes;
    const struct ds_fmt *fmt;
    struct ds_voice *v;
};

static inline void fetch_frame(const struct ds_src *s, DWORD idx, float *out)
{
    const struct ds_fmt *f = s->fmt;
    int ch = f->ch;
    if (f->adpcm) {
        int32_t block = (int32_t)(idx / 64);
        struct ds_voice *v = s->v;
        if (v->adpcm_block != block || v->adpcm_base != s->data) {
            ds_adpcm_decode_block(s->data + (DWORD)block * f->unit_bytes, ch, v->adpcm_cache);
            v->adpcm_block = block;
            v->adpcm_base = s->data;
        }
        const int16_t *c = v->adpcm_cache + (idx % 64) * ch;
        for (int i = 0; i < ch; i++) out[i] = c[i] * (1.0f / 32768.0f);
    } else if (f->bits == 16) {
        const uint8_t *p = s->data + idx * f->align;
        for (int i = 0; i < ch; i++) {
            int16_t x; memcpy(&x, p + 2 * i, 2);
            out[i] = x * (1.0f / 32768.0f);
        }
    } else {
        const uint8_t *p = s->data + idx * f->align;
        for (int i = 0; i < ch; i++) out[i] = (p[i] - 128) * (1.0f / 128.0f);
    }
}

/* ---- volume, mixbins, 3D --------------------------------------------------- */

/* Mixbin -> stereo fold: left and right weight of each bin. */
static const float bin_fold[DSMIXBIN_COUNT][2] = {
    { 1, 0 }, { 0, 1 }, { 0.707f, 0.707f }, { 0.5f, 0.5f }, { 0.707f, 0 }, { 0, 0.707f },
    { 1, 0 }, { 0, 1 }, { 0.707f, 0 }, { 0, 0.707f },
};

static const DSMIXBINVOLUMEPAIR default_bins_stereo[] = { { 0, 0 }, { 1, 0 } };
static const DSMIXBINVOLUMEPAIR default_bins_4ch[] = { { 0, 0 }, { 1, 0 }, { 4, 0 }, { 5, 0 } };
static const DSMIXBINVOLUMEPAIR default_bins_6ch[] = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 0 } };
static const DSMIXBINVOLUMEPAIR default_bins_3d[] = { { 6, 0 }, { 8, 0 }, { 7, 0 }, { 9, 0 }, { 10, 0 } };

/* Default mixbins (CDirectSoundVoiceSettings::SetMixBins(NULL)): the 3D set for
   CTRL3D voices, else by nChannels / 2 (Mono and Stereo are both FL+FR). */
static void voice_default_bins(struct ds_voice *v)
{
    const DSMIXBINVOLUMEPAIR *p; DWORD n;
    if (v->flags & DSBCAPS_CTRL3D) { p = default_bins_3d; n = 5; }
    else if (v->fmt.ch >= 6) { p = default_bins_6ch; n = 6; }
    else if (v->fmt.ch >= 4) { p = default_bins_4ch; n = 4; }
    else { p = default_bins_stereo; n = 2; }
    v->nbins = n;
    for (DWORD i = 0; i < n; i++) { v->bins[i] = (uint8_t)p[i].dwMixBin; v->binvol[p[i].dwMixBin] = p[i].lVolume; }
}

/* A WAVE_FORMAT_EXTENSIBLE channel mask assigns a 2D voice one bin per set
   bit, lowest first, at full volume (CDirectSoundVoiceSettings::SetFormat). */
static void voice_mask_bins(struct ds_voice *v, DWORD channel_mask)
{
    v->nbins = 0;
    for (DWORD b = 0; b < 6 && v->nbins < DSMIXBIN_ASSIGNMENT_MAX; b++)
        if (channel_mask & (1u << b)) { v->bins[v->nbins++] = (uint8_t)b; v->binvol[b] = 0; }
}

/* SetMixBins on a voice that feeds a submix buffer keeps (or appends) the
   submix buffer's input bin, at full volume when appended. */
static void voice_keep_submix_bin(struct ds_voice *v)
{
    if (!v->output) return;
    uint8_t in = (uint8_t)v->output->input_mixbin;
    for (DWORD i = 0; i < v->nbins; i++) if (v->bins[i] == in) return;
    if (v->nbins >= DSMIXBIN_ASSIGNMENT_MAX) v->nbins = DSMIXBIN_ASSIGNMENT_MAX - 1;
    v->bins[v->nbins++] = in;
    v->binvol[in] = 0;
}

static float vnorm(float *x)
{
    float m = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (m > 0) { x[0] /= m; x[1] /= m; x[2] /= m; }
    return m;
}

static float vlen(const float *x) { return sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); }

static void voice_calc_3d(struct ds_voice *v)
{
    memset(v->pan, 0, sizeof(v->pan));
    v->l3d_vol = 0; v->l3d_doppler = 0;
    if (!(v->flags & DSBCAPS_CTRL3D) || !g.ds || v->p3d.mode == DS3DMODE_DISABLE) return;
    const struct ds_3d *s = &v->p3d;
    const struct ds_listener *l = &g.ds->listener;
    int headrel = s->mode == DS3DMODE_HEADRELATIVE;
    /* Direction and distance from the listener in world space (CalcPosition). */
    float wn[3] = { s->pos[0], s->pos[1], s->pos[2] };
    if (!headrel) for (int i = 0; i < 3; i++) wn[i] -= l->pos[i];
    float mag = vnorm(wn);

    /* Distance attenuation (CalcDistanceVolume): the listener's distance
       factor scales velocities only, not this distance. */
    LONG dist_vol = 0;
    if (mag > s->min_dist) {
        float d = mag > s->max_dist ? s->max_dist : mag;
        float x = s->rolloff * l->rolloff * (d / s->min_dist - 1.0f);
        dist_vol = x >= 0 ? (LONG)lrintf(-2000.0f * log10f(x + 1.0f)) : 0;
        if (dist_vol < DSBVOLUME_MIN) dist_vol = DSBVOLUME_MIN;
    }
    /* Cone (CalcDirection / CalcConeVolume): flThetaS approximates the full
       angle (0..360) between the cone orientation and the direction from
       the source to the listener, as the cone angles are full apex angles. */
    LONG cone_vol = 0;
    const float *c = s->cone_dir;
    if (s->cone_in < 360 && (c[0] || c[1] || c[2])) {
        float cm[3] = { c[0] - wn[0], c[1] - wn[1], c[2] - wn[2] };
        float cp[3] = { c[0] + wn[0], c[1] + wn[1], c[2] + wn[2] };
        float acm = vlen(cm), acp = vlen(cp), theta;
        if (acp < acm) theta = 4.0f * (acp / acm * 45.0f);
        else theta = acp > 0 ? 4.0f * (90.0f - acm / acp * 45.0f) : 0.0f;
        DWORD in = s->cone_in, out = s->cone_out;
        if (theta <= (float)in) cone_vol = 0;
        else if (in < out && theta >= (float)out) cone_vol = s->cone_vol;
        else cone_vol = (LONG)lrintf((float)s->cone_vol * (theta - (float)in) / (float)(out > in ? out - in : 1));
    }
    v->l3d_vol = dist_vol + cone_vol;
    if (v->l3d_vol < DSBVOLUME_MIN) v->l3d_vol = DSBVOLUME_MIN;

    /* Doppler (CalcDoppler), along the world-space direction. */
    float vel = 0;
    for (int i = 0; i < 3; i++) vel += (s->vel[i] - (headrel ? 0.0f : l->vel[i])) * wn[i];
    vel *= s->dist_factor * l->dist_factor;
    vel *= s->doppler * l->doppler;
    if (vel == 0) v->l3d_doppler = 0;
    else if (vel >= 342.0f) v->l3d_doppler = DSBPITCH_MIN;
    else if (vel <= -342.0f) v->l3d_doppler = 4096;
    else v->l3d_doppler = (LONG)lrint(4096.0 * log2(1.0 - vel / 342.0));

    /* Pan3D (CPan3dSource::CalcPan): per speaker, -6400 * |(dir - speaker)/2|^2.
       The speakers sit around the listener, so the direction is taken into
       listener space (z = front, y = top, x = right) instead of rotating the
       speakers into world space; head-relative sources are already there. */
    static const struct { float pos[3]; int bin; } speakers[5] = {
        { { -0.7f, 0, 0.7f }, 6 }, { { 0.7f, 0, 0.7f }, 7 }, { { -0.7f, 0, -0.7f }, 8 }, { { 0.7f, 0, -0.7f }, 9 },
        { { 0, 0, 1 }, 2 },
    };
    if (mag > 0) {
        float dir[3] = { wn[0], wn[1], wn[2] };
        if (!headrel) {
            float z[3] = { l->front[0], l->front[1], l->front[2] };
            float y[3] = { l->top[0], l->top[1], l->top[2] };
            if (vnorm(z) > 0 && vnorm(y) > 0) {
                float d = y[0] * z[0] + y[1] * z[1] + y[2] * z[2];
                for (int i = 0; i < 3; i++) y[i] -= d * z[i];
                if (vnorm(y) > 0) {
                    float x[3] = { y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0] };
                    float r[3] = { wn[0] * x[0] + wn[1] * x[1] + wn[2] * x[2],
                                   wn[0] * y[0] + wn[1] * y[1] + wn[2] * y[2],
                                   wn[0] * z[0] + wn[1] * z[1] + wn[2] * z[2] };
                    memcpy(dir, r, sizeof(dir));
                }
            }
        }
        /* The library's stereo Pan3D sets only the front pair and leaves the
           rear 3D bins at full volume for the DSP (crosstalk / HRTF) to place;
           we have no DSP, so the rear pair is panned too and folded into the
           stereo output, which keeps left/right and front/back cues audible. */
        int n = (g.ds->speaker_config & 0xFFFF) == 2 ? 5 : 4;   /* DSSPEAKER_SURROUND adds the center */
        for (int i = 0; i < n; i++) {
            float dx = (dir[0] - speakers[i].pos[0]) / 2, dy = (dir[1] - speakers[i].pos[1]) / 2,
                  dz = (dir[2] - speakers[i].pos[2]) / 2;
            LONG vol = (LONG)((dx * dx + dy * dy + dz * dz) * -6400.0f);
            if (vol < DSBVOLUME_MIN) vol = DSBVOLUME_MIN;
            if (vol > 0) vol = 0;
            v->pan[speakers[i].bin] = vol;
        }
    }
}

/* Source channel that mixbin slot k of the voice carries.  The hardware
   plays a voice of more than two channels as ((ch-1)>>1)+1 stereo voices,
   gives each an equal share of the slots in order, and a stereo hardware
   voice sends its left channel to the even slots and its right channel to
   the odd ones (ConvertVolumeValues; CMcpxVoiceClient::SetFormat). */
static int voice_slot_channel(const struct ds_voice *v, DWORD k)
{
    int ch = v->fmt.ch ? v->fmt.ch : 1;
    if (ch == 1) return 0;
    DWORD hw = (DWORD)((ch - 1) >> 1) + 1, per = v->nbins / hw;
    DWORD voice = per ? k / per : 0;
    if (voice >= hw) voice = hw - 1;
    int c = (int)(2 * voice + (k & 1));
    return c < ch ? c : ch - 1;
}

static void voice_calc_gains(struct ds_voice *v)
{
    int ch = v->fmt.ch ? v->fmt.ch : 1;
    memset(v->gain, 0, sizeof(v->gain));
    memset(v->subgain, 0, sizeof(v->subgain));
    int sub_bin = v->output ? (int)v->output->input_mixbin : -1;
    for (DWORD k = 0; k < v->nbins; k++) {
        int c = voice_slot_channel(v, k);
        if (c >= 6) continue;
        int b = v->bins[k];
        LONG att = -v->volume - v->binvol[b] - v->l3d_vol - v->pan[b];
        if (att < 0) att = 0;
        float gain = att >= 6400 ? 0.0f : powf(10.0f, -(float)att / 2000.0f);
        gain = ldexpf(gain, -(g.ds ? g.ds->mixbin_headroom[b] : (b == DSMIXBIN_SUBMIX ? 0 : 1)));
        if (b == sub_bin) {
            /* SetOutputBuffer: the bin feeds the (mono) submix buffer. */
            v->subgain[c] += gain;
            continue;
        }
        v->gain[c][0] += gain * bin_fold[b][0];
        v->gain[c][1] += gain * bin_fold[b][1];
    }
    (void)ch;
}

static void voice_refresh(struct ds_voice *v)
{
    if (!v->dirty) return;
    voice_calc_3d(v);
    voice_calc_gains(v);
    v->dirty = 0;
}

static LONG voice_effective_pitch(const struct ds_voice *v)
{
    LONG p = v->pitch + v->l3d_doppler;
    if (v->output) p += v->output->v.pitch + v->output->v.l3d_doppler;
    if (p < DSBPITCH_MIN) p = DSBPITCH_MIN;
    if (p > DSBPITCH_MAX) p = DSBPITCH_MAX;
    return p;
}

/* ---- voice processor: envelopes and filter ----------------------------------- */

/* The model is the one bootani's software mixer uses (dsound_soft.c):
   envelope lengths count 512-sample units; attack is linear, decay and
   release exponential, landing at the end of their programmed length.  The
   multi-function EG moves pitch by lPitchScale (s.7 octaves at full scale)
   and the filter cutoff by lFilterCutOff (s3.4 octaves).  DLS2 filter
   coefficient 0 is log2 of a Chamberlin SVF frequency coefficient in s3.12,
   coefficient 1 its damping in 1.15; the MCPX's own fixed-point filter is not
   documented, so this is an approximation.  Parameters update every 32
   frames, as the APU processes voices in 32-sample frames. */
enum { DSEG_IDX_MULTI = 0, DSEG_IDX_AMP = 1 };
enum { EGD_EG, EGD_MODE, EGD_DELAY, EGD_ATTACK, EGD_HOLD, EGD_DECAY, EGD_RELEASE, EGD_SUSTAIN, EGD_PITCH, EGD_FC };
enum { EG_OFF, EG_DELAY, EG_ATTACK, EG_HOLD, EG_DECAY, EG_SUSTAIN, EG_RELEASE };
#define EG_LEN(x) (((uint32_t)(x) & 0xFFFu) * 512u)

static void eg_enter(struct ds_eg *e, int seg)
{
    for (;;) {
        e->seg = seg;
        switch (seg) {
        case EG_DELAY:
            e->value = 0.0f;
            if ((e->count = EG_LEN(e->d[EGD_DELAY]))) return;
            seg = EG_ATTACK; break;
        case EG_ATTACK:
            if ((e->count = EG_LEN(e->d[EGD_ATTACK]))) { e->step = (1.0f - e->value) / e->count; return; }
            seg = EG_HOLD; break;
        case EG_HOLD:
            e->value = 1.0f;
            if ((e->count = EG_LEN(e->d[EGD_HOLD]))) return;
            seg = EG_DECAY; break;
        case EG_DECAY:
            e->target = (e->d[EGD_SUSTAIN] & 0xFF) / 256.0f;
            if ((e->count = EG_LEN(e->d[EGD_DECAY]))) { e->coef = (float)exp(-5.0 / e->count); return; }
            seg = EG_SUSTAIN; break;
        case EG_SUSTAIN:
            e->value = (e->d[EGD_SUSTAIN] & 0xFF) / 256.0f;
            e->count = 0;
            return;
        case EG_RELEASE:
            if ((e->count = EG_LEN(e->d[EGD_RELEASE]))) { e->coef = (float)exp(-5.0 / e->count); return; }
            seg = EG_OFF; break;
        default:
            e->seg = EG_OFF; e->value = 0.0f; e->count = 0;
            return;
        }
    }
}

static void eg_default(struct ds_eg *e, DWORD which)
{
    memset(e, 0, sizeof(*e));
    e->d[EGD_EG] = which;
    e->d[EGD_SUSTAIN] = 0xFF;
    e->seg = EG_SUSTAIN;
    e->value = 1.0f;
}

static void eg_start(struct ds_eg *e)
{
    switch (e->d[EGD_MODE]) {
    case 1: eg_enter(e, EG_DELAY); break;                        /* DSEG_MODE_DELAY */
    case 2: e->value = 0.0f; eg_enter(e, EG_ATTACK); break;      /* DSEG_MODE_ATTACK */
    case 3: eg_enter(e, EG_HOLD); break;                         /* DSEG_MODE_HOLD */
    default: e->seg = EG_SUSTAIN; e->value = 1.0f; e->count = 0; /* disabled: full scale */
    }
}

static float eg_tick(struct ds_eg *e)
{
    float v = e->value;
    switch (e->seg) {
    case EG_DELAY: if (--e->count == 0) eg_enter(e, EG_ATTACK); break;
    case EG_ATTACK: e->value += e->step; if (--e->count == 0) eg_enter(e, EG_HOLD); break;
    case EG_HOLD: if (--e->count == 0) eg_enter(e, EG_DECAY); break;
    case EG_DECAY:
        e->value = e->target + (e->value - e->target) * e->coef;
        if (--e->count == 0) eg_enter(e, EG_SUSTAIN);
        break;
    case EG_RELEASE: e->value *= e->coef; if (--e->count == 0) eg_enter(e, EG_OFF); break;
    }
    return v;
}

static float s16field(DWORD v) { return (float)(int16_t)(uint16_t)(v & 0xFFFF); }
static float s8field(DWORD v) { return (float)(int8_t)(uint8_t)(v & 0xFF); }

/* VoiceOn: restart both envelopes and the filter. */
static void voice_on(struct ds_voice *v)
{
    v->noteoff = 0;
    v->ctl = 0;
    eg_start(&v->eg[DSEG_IDX_AMP]);
    eg_start(&v->eg[DSEG_IDX_MULTI]);
    memset(v->low, 0, sizeof(v->low));
    memset(v->band, 0, sizeof(v->band));
    v->fc_cur = s16field(v->fc0);
    v->q_cur = (v->fc1 & 0xFFFF) / 32768.0f;
}

static bool voice_has_vp(const struct ds_voice *v)
{
    return v->fmode == 1 || v->eg[DSEG_IDX_AMP].d[EGD_MODE] || v->eg[DSEG_IDX_MULTI].d[EGD_MODE];
}

/* Per 32-frame update: pitch and cutoff with the multi-function EG. */
static void voice_frame_params(struct ds_voice *v, LONG pitch)
{
    const struct ds_eg *m = &v->eg[DSEG_IDX_MULTI];
    float env = m->value;
    float p = pitch + s8field(m->d[EGD_PITCH]) * 32.0f * env;
    if (p < DSBPITCH_MIN) p = DSBPITCH_MIN;
    if (p > DSBPITCH_MAX) p = DSBPITCH_MAX;
    v->step_cur = exp2(p / 4096.0);
    if (v->fmode == 1) {   /* DSFILTER_MODE_DLS2; other modes are passed through */
        v->fc_cur += (s16field(v->fc0) - v->fc_cur) * 0.25f;
        v->q_cur += ((v->fc1 & 0xFFFF) / 32768.0f - v->q_cur) * 0.25f;
        float fc = v->fc_cur + s8field(m->d[EGD_FC]) * 256.0f * env;
        if (fc > 0.0f) fc = 0.0f;
        if (fc < -32768.0f) fc = -32768.0f;
        v->svf_f = exp2f(fc / 4096.0f);
        v->svf_q = v->q_cur < 0.02f ? 0.02f : v->q_cur > 2.0f ? 2.0f : v->q_cur;
    }
}

static void voice_init(struct ds_voice *v, int kind, void *owner, DWORD flags, const struct ds_fmt *fmt,
                       DWORD channel_mask)
{
    memset(v, 0, sizeof(*v));
    v->kind = kind; v->owner = owner; v->flags = flags; v->fmt = *fmt;
    v->headroom = (flags & DSBCAPS_SUBMIXMASK) ? 0 : (flags & DSBCAPS_CTRL3D) ? 0 : DSBHEADROOM_DEFAULT_2D;
    v->volume = -(LONG)v->headroom;
    v->pitch = calc_pitch(fmt->rate);
    if (channel_mask && !(flags & DSBCAPS_CTRL3D)) voice_mask_bins(v, channel_mask);
    else voice_default_bins(v);
    v->p3d.cone_in = v->p3d.cone_out = 360;
    v->p3d.cone_dir[2] = 1.0f;
    v->p3d.min_dist = 1.0f; v->p3d.max_dist = 1000000000.0f;
    v->p3d.dist_factor = v->p3d.rolloff = v->p3d.doppler = 1.0f;
    v->p3d_def = v->p3d;
    v->adpcm_block = -1;
    v->dirty = 1;
    eg_default(&v->eg[DSEG_IDX_MULTI], 0);
    eg_default(&v->eg[DSEG_IDX_AMP], 1);
}

static HRESULT voice_set_mixbins(struct ds_voice *v, const DSMIXBINS *p)
{
    if (!p) { voice_default_bins(v); voice_keep_submix_bin(v); v->dirty = 1; return DS_OK; }
    if (p->dwMixBinCount > DSMIXBIN_ASSIGNMENT_MAX || (p->dwMixBinCount && !p->lpMixBinVolumePairs))
        return DSERR_INVALIDPARAM;
    for (DWORD i = 0; i < p->dwMixBinCount; i++) {
        if (p->lpMixBinVolumePairs[i].dwMixBin >= DSMIXBIN_COUNT) return DSERR_INVALIDPARAM;
        if (p->lpMixBinVolumePairs[i].lVolume < DSBVOLUME_MIN || p->lpMixBinVolumePairs[i].lVolume > 0)
            return DSERR_INVALIDPARAM;
    }
    v->nbins = p->dwMixBinCount;
    for (DWORD i = 0; i < p->dwMixBinCount; i++) {
        v->bins[i] = (uint8_t)p->lpMixBinVolumePairs[i].dwMixBin;
        v->binvol[v->bins[i]] = p->lpMixBinVolumePairs[i].lVolume;
    }
    voice_keep_submix_bin(v);
    v->dirty = 1;
    return DS_OK;
}

/* ---- time ------------------------------------------------------------------ */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* 48 kHz sample clock, interpolated between mixer blocks.  Lock held. */
static uint64_t sample_time_locked(void)
{
    uint64_t t = g.frames;
    if (g.block_ns && g.audio_mode && g.audio_mode != 3) {
        uint64_t el = (mono_ns() - g.block_ns) * MIX_RATE / 1000000000ull;
        if (el > MIX_FRAMES) el = MIX_FRAMES;
        t += el;
    }
    return t;
}

static int64_t reference_time_locked(void)
{
    return (int64_t)(sample_time_locked() * 10000000ull / MIX_RATE);
}

/* ---- notifications & completions ------------------------------------------- */

#define MAX_EVENTS 256
struct ds_events { HANDLE ev[MAX_EVENTS]; unsigned n; };

static void events_add(struct ds_events *e, HANDLE h)
{
    if (e && h && e->n < MAX_EVENTS) e->ev[e->n++] = h;
}

static void events_signal(struct ds_events *e)
{
    extern NTSTATUS NTAPI NtSetEvent(HANDLE, LONG *);
    for (unsigned i = 0; i < e->n; i++) NtSetEvent(e->ev[i], NULL);
    e->n = 0;
}

/* Buffer position notifications, as CMcpxBuffer::OnPositionDelta: the
   sorted entries are signalled in order while their offset is below the
   play cursor; a cursor that moved backwards is taken as a loop (signal to
   the end of the region, rewind to its start); once the voice has stopped,
   the OFFSETSTOP entries are signalled.  Lock held; events are queued. */
static DWORD buffer_cursor_locked(const struct ds_buffer *b)
{
    return b->playing ? frames_to_bytes(&b->v.fmt, (DWORD)b->pos) : b->cursor;
}

static void buffer_notify_to(struct ds_buffer *b, DWORD cursor, int signal, struct ds_events *ev)
{
    while (b->notify_next < b->nnotify) {
        DWORD off = b->notify[b->notify_next].offset;
        if (off == DSBPN_OFFSETSTOP || off >= cursor) break;
        if (signal) events_add(ev, b->notify[b->notify_next].event);
        b->notify_next++;
    }
    b->notify_last = cursor;
}

static void buffer_position_delta(struct ds_buffer *b, struct ds_events *ev)
{
    if (!b->nnotify) return;
    DWORD start = 0, end = b->play_len;
    if (b->playing && b->looping) { start = b->loop_start; end = b->loop_start + b->loop_len; }
    DWORD cur = buffer_cursor_locked(b);
    if (cur > b->notify_last || b->notify_last > end) {
        buffer_notify_to(b, cur, 1, ev);
    } else if (cur < b->notify_last) {
        buffer_notify_to(b, end, 1, ev);
        b->notify_next = 0;
        buffer_notify_to(b, start, 0, ev);
        buffer_notify_to(b, cur, 1, ev);
    }
    if (!b->playing) {
        b->notify_active = 0;
        for (DWORD i = b->nnotify; i > 0 && b->notify[i - 1].offset == DSBPN_OFFSETSTOP; i--)
            events_add(ev, b->notify[i - 1].event);
    }
}

static void done_push(struct ds_done **list, DWORD *n, DWORD *cap, const struct ds_done *d)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *list = realloc(*list, *cap * sizeof(**list));
    }
    (*list)[(*n)++] = *d;
}

/* Queue a completion.  Lock held; nothing is delivered here. */
static void stream_complete(struct ds_stream *s, const struct ds_packet *p, DWORD status)
{
    struct ds_done d = { p->xmp, status, s->callback, s->context, s };
    if (status == XMEDIAPACKET_STATUS_SUCCESS) g.packets_done++;
    if (s->v.flags & DSSTREAMCAPS_ACCURATENOTIFY) {
        done_push(&g.acc, &g.nacc, &g.acc_cap, &d);
        pthread_cond_signal(&g.ccond);
    } else {
        done_push(&s->done, &s->ndone, &s->done_cap, &d);
    }
}

/* Deliver completions to the title.  Lock NOT held; the stream may be released
   by a callback, so only the local copies are used. */
static void deliver(const struct ds_done *list, DWORD n)
{
    extern NTSTATUS NTAPI NtSetEvent(HANDLE, LONG *);
    for (DWORD i = 0; i < n; i++) {
        const struct ds_done *d = &list[i];
        if (d->xmp.pdwCompletedSize) *d->xmp.pdwCompletedSize = d->xmp.dwMaxSize;
        if (d->xmp.pdwStatus) *d->xmp.pdwStatus = d->status;
#ifdef XBC_TRANSLATED
        if (d->cb) CPU_CALL(d->cb, CONV_STD, (uint32_t)d->ctx, (uint32_t)d->xmp.pContext, d->status);
#else
        if (d->cb) d->cb(d->ctx, d->xmp.pContext, d->status);
#endif
        else if (d->xmp.pContext) NtSetEvent((HANDLE)d->xmp.pContext, NULL);
    }
}

/* Take a stream's queued (non-accurate) completions.  Lock held. */
static struct ds_done *stream_take_done(struct ds_stream *s, DWORD *n)
{
    struct ds_done *l = s->done;
    *n = s->ndone;
    s->done = NULL; s->ndone = s->done_cap = 0;
    return l;
}

/* Drain one stream's completions on the caller's thread (lock not held). */
static void stream_drain(struct ds_stream *s)
{
    LOCK();
    DWORD n; struct ds_done *l = stream_take_done(s, &n);
    UNLOCK();
    if (n) deliver(l, n);
    free(l);
}

static void flush_locked(struct ds_stream *s, DWORD status)
{
    while (s->count) {
        stream_complete(s, &s->pk[s->head], status);
        s->head = (s->head + 1) % s->max_packets;
        s->count--;
    }
    s->pos = 0;
    s->active = 0; s->paused = 0; s->starved = 0; s->discontinuity = 0; s->flush_pending = 0;
    s->v.adpcm_block = -1;
}

static void *completion_thread(void *arg)
{
    (void)arg;
    thread_adopt_host("dsound");
    LOCK();
    for (;;) {
        while (!g.nacc && !g.stopping) pthread_cond_wait(&g.ccond, &g.lock);
        if (g.stopping) break;
        struct ds_done *l = g.acc; DWORD n = g.nacc;
        g.acc = NULL; g.nacc = g.acc_cap = 0;
        UNLOCK();
        deliver(l, n);
        free(l);
        LOCK();
    }
    UNLOCK();
    return NULL;
}

/* ---- the mixer --------------------------------------------------------------- */

/* Render one buffer voice into out (interleaved stereo, n frames). */
static void render_buffer(struct ds_buffer *b, float *out, int n, struct ds_events *ev)
{
    struct ds_voice *v = &b->v;
    if (!b->playing || b->paused || !b->data || (v->flags & DSBCAPS_SUBMIXMASK)) return;
    const struct ds_fmt *f = &v->fmt;
    DWORD total = bytes_to_frames(f, b->play_len);
    if (!total) { b->playing = 0; b->cursor = 0; if (b->notify_active) buffer_position_delta(b, ev); return; }
    DWORD ls = bytes_to_frames(f, b->loop_start), ll = bytes_to_frames(f, b->loop_len);
    int looping = b->looping && ll > 0 && ls + ll <= total;
    DWORD le = ls + ll;
    voice_refresh(v);
    LONG pitch = voice_effective_pitch(v);
    double step = exp2((double)pitch / 4096.0);
    bool vp = voice_has_vp(v);
    struct ds_src src = { b->data + b->play_start, total, f, v };
    int ch = f->ch;
    float *sub = v->output ? v->output->sub : NULL;
    double pos = b->pos;
    float a[6], c[6];
    for (int i = 0; i < n; i++) {
        if (looping && pos >= le) pos = ls + fmod(pos - ls, (double)ll);
        if (pos >= total) { b->playing = 0; pos = 0; break; }
        DWORD idx = (DWORD)pos;
        float frac = (float)(pos - idx);
        DWORD nxt = idx + 1;
        if (looping && nxt >= le) nxt = ls;
        if (nxt >= total) nxt = idx;
        fetch_frame(&src, idx, a);
        fetch_frame(&src, nxt, c);
        float l = 0, r = 0, m = 0, amp = 1.0f;
        if (vp) {
            if ((v->ctl++ & 31) == 0) voice_frame_params(v, pitch);
            step = v->step_cur;
            amp = eg_tick(&v->eg[DSEG_IDX_AMP]);
            eg_tick(&v->eg[DSEG_IDX_MULTI]);
        }
        for (int k = 0; k < ch; k++) {
            float s = a[k] + (c[k] - a[k]) * frac;
            if (vp && v->fmode == 1) {   /* Chamberlin state-variable low-pass */
                v->low[k] += v->svf_f * v->band[k];
                float high = s - v->low[k] - v->svf_q * v->band[k];
                v->band[k] += v->svf_f * high;
                s = v->low[k];
            }
            s *= amp;
            l += s * v->gain[k][0];
            r += s * v->gain[k][1];
            m += s * v->subgain[k];
        }
        out[2 * i] += l;
        out[2 * i + 1] += r;
        if (sub) sub[i] += m;
        if (v->noteoff && v->eg[DSEG_IDX_AMP].seg == EG_OFF) {   /* end of release: voice off */
            b->playing = 0;
            break;
        }
        pos += step;
    }
    if (b->playing) {
        if (looping && pos >= le) pos = ls + fmod(pos - ls, (double)ll);
        b->pos = pos;
    } else {
        /* Reached the end of a non-looping play region: the voice is off
           and the cached cursor is the 0 that Play left there. */
        b->pos = 0; b->cursor = 0;
    }
    if (b->notify_active) buffer_position_delta(b, ev);
}

static void render_stream(struct ds_stream *s, float *out, int n)
{
    struct ds_voice *v = &s->v;
    if (!s->active || s->paused || s->starved || !s->count) return;
    const struct ds_fmt *f = &v->fmt;
    voice_refresh(v);
    double step = exp2((double)voice_effective_pitch(v) / 4096.0);
    int ch = f->ch;
    float *sub = v->output ? v->output->sub : NULL;
    float a[6], c[6];
    for (int i = 0; i < n; i++) {
        struct ds_packet *p = &s->pk[s->head];
        while (s->pos >= p->frames) {
            s->pos -= p->frames;
            stream_complete(s, p, XMEDIAPACKET_STATUS_SUCCESS);
            s->head = (s->head + 1) % s->max_packets;
            s->count--;
            v->adpcm_block = -1;
            if (!s->count) {
                if (s->discontinuity) { s->active = 0; s->discontinuity = 0; s->pos = 0; }
                else s->starved = 1;
                return;
            }
            p = &s->pk[s->head];
        }
        struct ds_src src = { p->xmp.pvBuffer, p->frames, f, v };
        DWORD idx = (DWORD)s->pos;
        float frac = (float)(s->pos - idx);
        DWORD nxt = idx + 1 < p->frames ? idx + 1 : idx;
        fetch_frame(&src, idx, a);
        fetch_frame(&src, nxt, c);
        float l = 0, r = 0, m = 0;
        for (int k = 0; k < ch; k++) {
            float smp = a[k] + (c[k] - a[k]) * frac;
            l += smp * v->gain[k][0];
            r += smp * v->gain[k][1];
            m += smp * v->subgain[k];
        }
        out[2 * i] += l;
        out[2 * i + 1] += r;
        if (sub) sub[i] += m;
        s->pos += step;
    }
}

/* A MIXIN/FXIN buffer is a voice whose input is what the voices routed to it
   (SetOutputBuffer) produced this block; it is always active, as the
   hardware's submix voices are.  FXIN input would pass through a DSP effect
   first; without a DSP it is mixed in dry. */
static void render_submix(struct ds_buffer *b, float *out, int n)
{
    struct ds_voice *v = &b->v;
    voice_refresh(v);
    for (int i = 0; i < n; i++) {
        out[2 * i] += b->sub[i] * v->gain[0][0];
        out[2 * i + 1] += b->sub[i] * v->gain[0][1];
    }
}

static void buffer_stop_locked(struct ds_buffer *b, struct ds_events *ev);
static void buffer_stopex_locked(struct ds_buffer *b, DWORD flags, struct ds_events *ev);
static HRESULT buffer_play_locked(struct ds_buffer *b, DWORD flags);

/* Mix one block of n frames into out (zeroed here).  Called from the audio
   thread, the clock thread, or a test. */
void ds_mix_block(float *out, int n)
{
    for (; n > MIX_FRAMES; n -= MIX_FRAMES, out += 2 * MIX_FRAMES) ds_mix_block(out, MIX_FRAMES);
    struct ds_events ev = { .n = 0 };
    memset(out, 0, (size_t)n * 2 * sizeof(float));
    LOCK();
    g.frames += (uint64_t)n;
    g.block_ns = mono_ns();
    int64_t now = reference_time_locked();
    for (unsigned i = 0; i < g.nobjs; i++) {
        struct ds_entry *e = &g.objs[i];
        if (e->kind == DS_BUFFER && ((struct ds_buffer *)e->obj)->sub)
            memset(((struct ds_buffer *)e->obj)->sub, 0, (size_t)n * sizeof(float));
    }
    for (unsigned i = 0; i < g.nobjs; i++) {
        struct ds_entry *e = &g.objs[i];
        if (e->kind == DS_BUFFER) {
            struct ds_buffer *b = e->obj;
            if (b->sub) continue;
            if (b->start_at && now >= b->start_at) {
                b->start_at = 0;
                buffer_play_locked(b, b->start_flags);
            }
            if (b->stop_at && now >= b->stop_at) { b->stop_at = 0; buffer_stopex_locked(b, b->stop_flags, &ev); }
            render_buffer(b, out, n, &ev);
        } else if (e->kind == DS_STREAM) {
            render_stream(e->obj, out, n);
        }
    }
    for (unsigned i = 0; i < g.nobjs; i++) {
        struct ds_entry *e = &g.objs[i];
        if (e->kind == DS_BUFFER && ((struct ds_buffer *)e->obj)->sub) render_submix(e->obj, out, n);
    }
    float peak = 0;
    for (int i = 0; i < 2 * n; i++) {
        float a = fabsf(out[i]);
        if (a > peak) peak = a;
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
    if (peak > g.peak) g.peak = peak;
    if (peak > 0) g.audible_frames += (uint64_t)n;
    UNLOCK();
    events_signal(&ev);
}

static void SDLCALL sdl_audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    ds_mix_block((float *)stream, len / (int)(2 * sizeof(float)));
}

static void *clock_thread(void *arg)
{
    (void)arg;
    static float scratch[MIX_FRAMES * 2];
    uint64_t next = mono_ns();
    while (!g.stopping) {
        next += (uint64_t)MIX_FRAMES * 1000000000ull / MIX_RATE;
        uint64_t now = mono_ns();
        if (next > now) {
            struct timespec ts = { (time_t)((next - now) / 1000000000ull), (long)((next - now) % 1000000000ull) };
            nanosleep(&ts, NULL);
        } else {
            next = now;
        }
        ds_mix_block(scratch, MIX_FRAMES);
    }
    return NULL;
}

static void audio_stop(void)
{
    xlog("DSound: %llu frames mixed (%llu in audible blocks), peak %.3f, %u buffer plays, %u packets completed",
         (unsigned long long)g.frames, (unsigned long long)g.audible_frames, g.peak, g.plays, g.packets_done);
    g.stopping = 1;
    if (g.audio_mode == 1 && g.dev) { SDL_CloseAudioDevice(g.dev); g.dev = 0; }
    pthread_cond_broadcast(&g.ccond);
}

static void audio_start(void)
{
    if (g.audio_mode) return;
    if (g_ds_no_audio_thread) { g.audio_mode = 3; return; }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = MIX_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = MIX_FRAMES;
    want.callback = sdl_audio_cb;
    for (int attempt = 0; attempt < 2 && !g.dev; attempt++) {
        if (attempt == 1) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            setenv("SDL_AUDIODRIVER", "dummy", 1);
        }
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            xlog("DSound: SDL audio init failed: %s", SDL_GetError());
            continue;
        }
        install_fault_handlers();
        g.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!g.dev) xlog("DSound: cannot open audio device (%s): %s", SDL_GetCurrentAudioDriver() ?
                         SDL_GetCurrentAudioDriver() : "no driver", SDL_GetError());
    }
    if (g.dev) {
        g.audio_mode = 1;
        xlog("DSound: audio on %s driver, %d Hz, %d channels, %d frames per block",
             SDL_GetCurrentAudioDriver(), have.freq, have.channels, have.samples);
        SDL_PauseAudioDevice(g.dev, 0);
    } else {
        g.audio_mode = 2;
        xlog("DSound: no audio device, running a silent clock");
        pthread_t t;
        pthread_create(&t, NULL, clock_thread, NULL);
        pthread_detach(t);
    }
    atexit(audio_stop);
}

/* ---- the DirectSound object -------------------------------------------------- */

static struct ds_object *ds_create_locked(void)
{
    if (g.ds) { g.ds->hdr.refs++; return g.ds; }
    struct ds_object *d = pool_alloc(sizeof(*d));
    if (!d) return NULL;
    d->valid_vptr = 0; d->valid_sig = 0x444E5344; /* 'DSND' */
    guest_vtbls();
    d->hdr.vtbl = refhdr_vtbl; d->hdr.refs = 1;
    d->speaker_config = g.override_speaker != 0xFFFFFFFFu ? g.override_speaker : 0;
    for (int i = 0; i < DSMIXBIN_COUNT; i++) d->mixbin_headroom[i] = i == DSMIXBIN_SUBMIX ? 0 : 1;
    d->listener.front[2] = 1.0f; d->listener.top[1] = 1.0f;
    d->listener.dist_factor = d->listener.rolloff = d->listener.doppler = 1.0f;
    d->listener_def = d->listener;
    g.ds = d;
    registry_add(DS_DSOUND, d, (uint32_t)d->iface);
    audio_start();
    xlog("DSound: DirectSound object created");
    return d;
}

static void ds_release_ref_locked(struct ds_object *d)
{
    if (!d || d->hdr.refs == 0) return;
    if (--d->hdr.refs) return;
    registry_remove(d);
    if (g.ds == d) g.ds = NULL;
    for (DWORD i = 0; i < d->fx_count; i++) free(d->fx_state[i]);
    free(d->fx_state); free(d->fx_state_size); free(d->fx_desc);
    pool_free(d);
    xlog("DSound: DirectSound object destroyed");
}

/* ---- buffers ------------------------------------------------------------------- */

/* CMcpxBuffer::Stop: cache the play cursor, turn the voice off and signal
   the positions passed up to here plus the OFFSETSTOP entries. */
static void buffer_stop_locked(struct ds_buffer *b, struct ds_events *ev)
{
    if (b->playing) {
        b->cursor = frames_to_bytes(&b->v.fmt, (DWORD)b->pos);
        b->playing = 0;
        buffer_position_delta(b, ev);
    }
    b->start_at = 0; b->stop_at = 0;
}

/* StopEx: DSBSTOPEX_ENVELOPE enters the release phase of the envelopes and
   the voice turns off when the amplitude envelope reaches zero; with
   DSBSTOPEX_RELEASEWAVEFORM the loop is also broken.  A voice whose
   envelopes are disabled has no release: with RELEASEWAVEFORM the play
   region runs to its end, otherwise it stops now.  Without ENVELOPE it is a
   plain Stop. */
static void buffer_stopex_locked(struct ds_buffer *b, DWORD flags, struct ds_events *ev)
{
    struct ds_voice *v = &b->v;
    if (b->playing && (flags & DSBSTOPEX_ENVELOPE) && voice_has_vp(v)) {
        if (flags & DSBSTOPEX_RELEASEWAVEFORM) b->looping = 0;
        if (!v->noteoff) {
            v->noteoff = 1;
            if (v->eg[DSEG_IDX_AMP].seg != EG_OFF) eg_enter(&v->eg[DSEG_IDX_AMP], EG_RELEASE);
            if (v->eg[DSEG_IDX_MULTI].seg != EG_OFF) eg_enter(&v->eg[DSEG_IDX_MULTI], EG_RELEASE);
            if (v->eg[DSEG_IDX_AMP].seg == EG_OFF) buffer_stop_locked(b, ev);
        }
    } else if (b->playing && (flags & DSBSTOPEX_ENVELOPE) && (flags & DSBSTOPEX_RELEASEWAVEFORM)) {
        b->looping = 0;
    } else {
        buffer_stop_locked(b, ev);
    }
}

static void buffer_set_regions_locked(struct ds_buffer *b)
{
    b->play_start = 0; b->play_len = b->size;
    b->loop_start = 0; b->loop_len = b->size;
    b->cursor = 0; b->pos = 0;
    b->v.adpcm_block = -1;
}

static HRESULT buffer_create_locked(const DSBUFFERDESC *desc, uint32_t *pp)
{
    if (!desc || !pp) return DSERR_INVALIDPARAM;
    if (desc->dwSize < sizeof(DSBUFFERDESC) && desc->dwSize != 0) {
        xlog("DSound: DSBUFFERDESC size %u", desc->dwSize);
    }
    if (desc->dwFlags & ~DSBCAPS_VALID) {
        /* Later XDKs add flags (0x400000 in 5849); their buffers still play. */
        static ULONG warned;
        if ((desc->dwFlags & ~DSBCAPS_VALID) & ~warned) xlog("DSound: ignoring unknown buffer flags %#x", desc->dwFlags & ~DSBCAPS_VALID);
        warned |= desc->dwFlags & ~DSBCAPS_VALID;
    }
    struct ds_fmt fmt; DWORD mask = 0;
    int submix = !!(desc->dwFlags & DSBCAPS_SUBMIXMASK);
    if (submix) {
        if (desc->lpwfxFormat || desc->dwBufferBytes) return DSERR_INVALIDPARAM;
        if ((desc->dwFlags & DSBCAPS_FXIN) && (desc->dwInputMixBin < 11 || desc->dwInputMixBin > 30))
            return DSERR_INVALIDPARAM;
        memset(&fmt, 0, sizeof(fmt));
        fmt.tag = WAVE_FORMAT_PCM; fmt.ch = 1; fmt.bits = 16; fmt.align = 2; fmt.rate = MIX_RATE;
        fmt.unit_bytes = 2; fmt.unit_frames = 1;
    } else {
        HRESULT hr = fmt_parse(desc->lpwfxFormat, &fmt, &mask);
        if (hr != DS_OK) { xlog("DSound: invalid buffer format"); return hr; }
        if (desc->dwBufferBytes && (desc->dwBufferBytes < 4 || desc->dwBufferBytes > DSBSIZE_MAX ||
                                    desc->dwBufferBytes % fmt.align))
            return DSERR_INVALIDPARAM;
        if ((desc->dwFlags & DSBCAPS_CTRL3D) && fmt.ch != 1)
            xlog("DSound: 3D buffer with %u channels (library requires mono)", fmt.ch);
    }
    struct ds_object *ds = ds_create_locked();   /* implicit singleton; holds a ref for us */
    if (!ds) return DSERR_OUTOFMEMORY;
    struct ds_buffer *b = pool_alloc(sizeof(*b));
    if (!b) { ds_release_ref_locked(ds); return DSERR_OUTOFMEMORY; }
    b->valid_vptr = 0; b->valid_sig = 0x20425344; /* 'DSB ' */
    guest_vtbls();
    b->hdr.vtbl = refhdr_vtbl; b->hdr.refs = 1;
    voice_init(&b->v, DS_BUFFER, b, desc->dwFlags, &fmt, mask);
    if (!submix && desc->lpMixBins) {
        HRESULT hr = voice_set_mixbins(&b->v, desc->lpMixBins);
        if (hr != DS_OK) { ds_release_ref_locked(ds); pool_free(b); return hr; }
    }
    b->input_mixbin = (desc->dwFlags & DSBCAPS_FXIN) ? desc->dwInputMixBin : DSMIXBIN_SUBMIX;
    if (submix) b->sub = calloc(MIX_FRAMES, sizeof(float));
    if (desc->dwBufferBytes) {
        b->data = pool_alloc(desc->dwBufferBytes);
        if (!b->data) { ds_release_ref_locked(ds); pool_free(b); return DSERR_OUTOFMEMORY; }
        b->size = desc->dwBufferBytes;
        g.mem_allocated += b->size;
        buffer_set_regions_locked(b);
    }
    registry_add(DS_BUFFER, b, (uint32_t)b->iface);
    *pp = (uint32_t)b->iface;
    xlog("DSound: buffer %p created (flags %#x, %s %u ch %u Hz, %u bytes)", b->iface, desc->dwFlags,
         fmt.adpcm ? "adpcm" : "pcm", fmt.ch, fmt.rate, desc->dwBufferBytes);
    return DS_OK;
}

static void buffer_free_locked(struct ds_buffer *b);

/* Drop one reference on a buffer; the last one stops and frees it. */
static ULONG buffer_release_locked(struct ds_buffer *b, struct ds_events *ev)
{
    ULONG r = b->hdr.refs ? --b->hdr.refs : 0;
    if (!r) {
        buffer_stop_locked(b, ev);
        xlog("DSound: buffer %p released", b->iface);
        buffer_free_locked(b);
    }
    return r;
}

static void buffer_free_locked(struct ds_buffer *b)
{
    registry_remove(b);
    /* Nobody may route into a destroyed buffer. */
    for (unsigned i = 0; i < g.nobjs; i++) {
        struct ds_voice *v = NULL;
        if (g.objs[i].kind == DS_BUFFER) v = &((struct ds_buffer *)g.objs[i].obj)->v;
        else if (g.objs[i].kind == DS_STREAM) v = &((struct ds_stream *)g.objs[i].obj)->v;
        if (v && v->output == b) v->output = NULL;
    }
    if (b->data && !b->app_owned) { g.mem_allocated -= b->size; pool_free(b->data); }
    free(b->notify);
    free(b->sub);
    struct ds_buffer *out = b->v.output;
    b->v.output = NULL;
    ds_release_ref_locked(g.ds);
    pool_free(b);
    if (out) buffer_release_locked(out, NULL);
}

/* ---- streams ------------------------------------------------------------------- */

static void *stream_vtbl[7];

static HRESULT stream_create_locked(const DSSTREAMDESC *desc, uint32_t *pp)
{
    if (!desc || !pp) return DSERR_INVALIDPARAM;
    if (desc->dwFlags & ~DSSTREAMCAPS_VALID) return DSERR_INVALIDPARAM;
    if (!desc->dwMaxAttachedPackets) return DSERR_INVALIDPARAM;
    struct ds_fmt fmt; DWORD mask = 0;
    HRESULT hr = fmt_parse(desc->lpwfxFormat, &fmt, &mask);
    if (hr != DS_OK) { xlog("DSound: invalid stream format"); return hr; }
    struct ds_object *ds = ds_create_locked();
    if (!ds) return DSERR_OUTOFMEMORY;
    struct ds_stream *s = pool_alloc(sizeof(*s));
    if (!s) { ds_release_ref_locked(ds); return DSERR_OUTOFMEMORY; }
    guest_vtbls();
    s->vtbl = stream_vtbl;
    s->hdr_rel.vtbl = s->hdr_dbg.vtbl = refhdr_vtbl;
    s->hdr_rel.refs = s->hdr_dbg.refs = 1;
    voice_init(&s->v, DS_STREAM, s, desc->dwFlags, &fmt, mask);
    if (desc->lpMixBins) {
        hr = voice_set_mixbins(&s->v, desc->lpMixBins);
        if (hr != DS_OK) { ds_release_ref_locked(ds); pool_free(s); return hr; }
    }
    s->max_packets = desc->dwMaxAttachedPackets;
    s->pk = calloc(s->max_packets, sizeof(*s->pk));
    s->callback = desc->lpfnCallback; s->context = desc->lpvContext;
    s->active = 0;   /* the voice starts with the first packet (CMcpxStream::CommitSsl) */
    if ((desc->dwFlags & DSSTREAMCAPS_ACCURATENOTIFY) && !g.cthread_started && !g_ds_no_audio_thread) {
        pthread_t t;
        g.cthread_started = 1;
        pthread_create(&t, NULL, completion_thread, NULL);
        pthread_detach(t);
    }
    registry_add(DS_STREAM, s, (uint32_t)s);
    *pp = (uint32_t)s;
    xlog("DSound: stream %p created (flags %#x, %s %u ch %u Hz, %u packets)", s, desc->dwFlags,
         fmt.adpcm ? "adpcm" : "pcm", fmt.ch, fmt.rate, s->max_packets);
    return DS_OK;
}

static void stream_set_refs(struct ds_stream *s, uint32_t r) { s->hdr_rel.refs = s->hdr_dbg.refs = r; }

/* ---- IUnknown-ish ------------------------------------------------------------------ */

static ULONG NTAPI Obj_AddRef(void *self)
{
    LOCK();
    struct ds_entry *e = ds_find(self);
    ULONG r = 0;
    if (!e) xlog("DSound: AddRef on unknown object %p", self);
    else if (e->kind == DS_DSOUND) r = ++((struct ds_object *)e->obj)->hdr.refs;
    else if (e->kind == DS_BUFFER) r = ++((struct ds_buffer *)e->obj)->hdr.refs;
    else { struct ds_stream *s = e->obj; stream_set_refs(s, s->hdr_rel.refs + 1); r = s->hdr_rel.refs; }
    UNLOCK();
    return r;
}

static ULONG NTAPI Obj_Release(void *self)
{
    struct ds_events ev = { .n = 0 };
    struct ds_done *dl = NULL; DWORD dn = 0;
    LOCK();
    struct ds_entry *e = ds_find(self);
    ULONG r = 0;
    if (!e) {
        xlog("DSound: Release on unknown object %p", self);
    } else if (e->kind == DS_DSOUND) {
        struct ds_object *d = e->obj;
        r = d->hdr.refs ? d->hdr.refs - 1 : 0;
        ds_release_ref_locked(d);
    } else if (e->kind == DS_BUFFER) {
        r = buffer_release_locked(e->obj, &ev);
    } else {
        struct ds_stream *s = e->obj;
        r = s->hdr_rel.refs ? s->hdr_rel.refs - 1 : 0;
        stream_set_refs(s, r);
        if (!r) {
            /* Completions already queued for this stream go first, in order
               (ACCURATENOTIFY ones are taken out of the shared queue without
               reordering the other streams'), then the flushed packets. */
            DWORD cap = 0, keep = 0;
            for (DWORD i = 0; i < g.nacc; i++) {
                if (g.acc[i].s == s) done_push(&dl, &dn, &cap, &g.acc[i]);
                else g.acc[keep++] = g.acc[i];
            }
            g.nacc = keep;
            for (DWORD i = 0; i < s->ndone; i++) done_push(&dl, &dn, &cap, &s->done[i]);
            s->ndone = 0;
            flush_locked(s, XMEDIAPACKET_STATUS_FLUSHED);
            for (DWORD i = 0; i < s->ndone; i++) done_push(&dl, &dn, &cap, &s->done[i]);
            for (DWORD i = 0; i < g.nacc;) {   /* flushed ACCURATENOTIFY packets */
                if (g.acc[i].s == s) { done_push(&dl, &dn, &cap, &g.acc[i]); memmove(&g.acc[i], &g.acc[i + 1], (g.nacc - i - 1) * sizeof(*g.acc)); g.nacc--; }
                else i++;
            }
            free(s->done);
            registry_remove(s);
            free(s->pk);
            if (s->v.output) { buffer_release_locked(s->v.output, &ev); s->v.output = NULL; }
            ds_release_ref_locked(g.ds);
            xlog("DSound: stream %p released", s);
            pool_free(s);
        }
    }
    UNLOCK();
    events_signal(&ev);
    if (dn) deliver(dl, dn);
    free(dl);
    return r;
}

static HRESULT NTAPI Obj_QueryInterface(void *self, const void *iid, void **ppv)
{
    (void)iid;
    if (!ppv) return DSERR_INVALIDPARAM;
    LOCK();
    struct ds_entry *e = ds_find(self);
    uint32_t p = e ? e->p : 0;
    UNLOCK();
    if (!e) return DSERR_INVALIDPARAM;
    Obj_AddRef((void *)p);
    *ppv = (void *)p;
    return DS_OK;
}

/* ---- global functions ----------------------------------------------------------------- */

static HRESULT NTAPI DirectSoundCreate(const void *guid, uint32_t *pp, void *unk)
{
    if (!pp) return DSERR_INVALIDPARAM;
    if (unk) return DSERR_INVALIDPARAM;
    (void)guid;
    LOCK();
    struct ds_object *d = ds_create_locked();
    if (d) *pp = (uint32_t)d->iface;
    UNLOCK();
    return d ? DS_OK : DSERR_OUTOFMEMORY;
}

static HRESULT NTAPI DirectSoundCreateBuffer(const DSBUFFERDESC *desc, uint32_t *pp)
{
    LOCK();
    HRESULT hr = buffer_create_locked(desc, pp);
    UNLOCK();
    return hr;
}

static HRESULT NTAPI DirectSoundCreateStream(const DSSTREAMDESC *desc, uint32_t *pp)
{
    LOCK();
    HRESULT hr = stream_create_locked(desc, pp);
    UNLOCK();
    return hr;
}

static void do_work(void)
{
    /* Collect every deferred completion, then deliver without the lock. */
    struct ds_done *all = NULL; DWORD n = 0, cap = 0;
    LOCK();
    for (unsigned i = 0; i < g.nobjs; i++) {
        if (g.objs[i].kind != DS_STREAM) continue;
        struct ds_stream *s = g.objs[i].obj;
        if (s->flush_pending) flush_locked(s, XMEDIAPACKET_STATUS_FLUSHED);
        for (DWORD k = 0; k < s->ndone; k++) done_push(&all, &n, &cap, &s->done[k]);
        s->ndone = 0;
    }
    if (!g.cthread_started) {
        /* No completion thread (tests): ACCURATENOTIFY streams are served here too. */
        for (DWORD k = 0; k < g.nacc; k++) done_push(&all, &n, &cap, &g.acc[k]);
        g.nacc = 0;
    }
    UNLOCK();
    if (n) deliver(all, n);
    free(all);
}

static void NTAPI DirectSoundDoWork(void) { do_work(); }
static void NTAPI DS_Force3dRecalc(void *self, DWORD flags)
{
    (void)flags;
    LOCK();
    if (find_ds(self)) mark_all_3d_dirty();
    UNLOCK();
}
static void NTAPI DS_DoWork(void *self) { (void)self; do_work(); }

static DWORD NTAPI DirectSoundGetSampleTime(void)
{
    LOCK();
    DWORD t = g.ds ? (DWORD)sample_time_locked() : 0;
    UNLOCK();
    return t;
}

static void set_algorithm(int a, const char *name)
{
    LOCK();
    if (g.algorithm && g.algorithm != a) xlog("DSound: 3D algorithm changed to %s", name);
    g.algorithm = a;
    UNLOCK();
}
static void NTAPI DirectSoundUseFullHRTF(void) { set_algorithm(1, "full HRTF"); }
static void NTAPI DirectSoundUseLightHRTF(void) { set_algorithm(2, "light HRTF"); }
static void NTAPI DirectSoundUsePan3D(void) { set_algorithm(3, "Pan3D"); }

static void NTAPI DirectSoundOverrideSpeakerConfig(DWORD cfg)
{
    LOCK();
    if (cfg == 0xFFFFFFFFu) g.override_speaker = cfg;
    else if ((cfg & 0xFFFF0000u) == 0 && (cfg & 0xFFFF) <= 2) g.override_speaker = cfg & 0xFFFF;
    else xlog("DSound: invalid speaker config override %#x", cfg);
    UNLOCK();
}

static void NTAPI DirectSoundDumpMemoryUsage(ULONG assert_none) { (void)assert_none; }

static HRESULT NTAPI DirectSoundLoadEncoder(const char *name, DWORD flags, void **ppv, DWORD *psize)
{
    (void)name; (void)flags; (void)ppv; (void)psize;
    return DSERR_UNSUPPORTED;
}

/* The library's allocators.  Our own objects never use them, but library code
   that still runs (the WMA decoder XMOs) does.  Pool memory comes from the
   kernel pool, physical memory from the contiguous window. */
static void *ds_pool_alloc(DWORD size, ULONG zero) { (void)zero; return pool_alloc(size ? size : 1); }
static void ds_free(void *p)
{
    if (!p) return;
    if ((uint32_t)p >= CONTIG_BASE) MmFreeContiguousMemory(p);
    else pool_free(p);
}
static void *NTAPI DirectSoundMemAlloc(DWORD tag, DWORD size, ULONG zero) { (void)tag; return ds_pool_alloc(size, zero); }
static void *NTAPI DirectSoundTrackingAlloc(const char *file, ULONG line, const char *cls, DWORD tag, DWORD size,
                                           ULONG zero)
{
    (void)file; (void)line; (void)cls; (void)tag;
    return ds_pool_alloc(size, zero);
}
static void *NTAPI DirectSoundPhysicalAlloc(DWORD size, DWORD align, DWORD flags, ULONG zero)
{
    void *p = MmAllocateContiguousMemoryEx(size ? size : 1, 0, 0xFFFFFFFFu, align, flags ? flags : 4);
    if (p && zero) memset(p, 0, size);
    return p;
}
static void *NTAPI DirectSoundTrackingPhysicalAlloc(const char *file, ULONG line, const char *cls, DWORD size,
                                                   DWORD align, DWORD flags, ULONG zero)
{
    (void)file; (void)line; (void)cls;
    return DirectSoundPhysicalAlloc(size, align, flags, zero);
}
static void NTAPI DirectSoundMemFree(void *p) { ds_free(p); }

static LONG NTAPI XAudioCalculatePitch(DWORD freq) { return calc_pitch(freq); }

static void NTAPI XAudioCreatePcmFormat(uint16_t ch, DWORD rate, uint16_t bits, WAVEFORMATEX *w)
{
    if (!w) return;
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, ch, rate, rate * ch * bits / 8, (uint16_t)(ch * bits / 8), bits, 0 };
    memcpy(w, &f, sizeof(f));
}

static void NTAPI XAudioCreateAdpcmFormat(uint16_t ch, DWORD rate, XBOXADPCMWAVEFORMAT *w)
{
    if (!w) return;
    XBOXADPCMWAVEFORMAT f = { { WAVE_FORMAT_XBOX_ADPCM, ch, rate, rate / 64 * 36, (uint16_t)(36 * ch), 4, 2 }, 64 };
    memcpy(w, &f, sizeof(f));
}

/* ---- effects image ---------------------------------------------------------------------- */

static HRESULT fx_download_locked(struct ds_object *d, const void *pv, DWORD size, const DSEFFECTIMAGELOC *loc,
                                  DSEFFECTIMAGEDESC **ppDesc)
{
    if (!pv || size <= 2048 + 24) return DSERR_INVALIDPARAM;
    const uint8_t *img = pv;
    uint32_t cmd[6]; memcpy(cmd, img + 2048, sizeof(cmd));
    uint64_t off = 2048 + 24 + (uint64_t)cmd[1] * 4 + (uint64_t)cmd[3] * 4;
    DWORD count = 0;
    const DSEFFECTIMAGEDESC *src = NULL;
    if (off + 8 <= size) {
        src = (const DSEFFECTIMAGEDESC *)(img + off);
        count = src->dwEffectCount;
        if (count > 256 || off + 8 + 32ull * count > size) { count = 0; src = NULL; }
    }
    for (DWORD i = 0; i < d->fx_count; i++) free(d->fx_state[i]);
    free(d->fx_state); free(d->fx_state_size); free(d->fx_desc);
    size_t dsize = 8 + 32 * (size_t)count;
    d->fx_desc = calloc(1, dsize < sizeof(DSEFFECTIMAGEDESC) ? sizeof(DSEFFECTIMAGEDESC) : dsize);
    if (src) memcpy(d->fx_desc, src, dsize);
    d->fx_count = count;
    d->fx_state = calloc(count ? count : 1, sizeof(*d->fx_state));
    d->fx_state_size = calloc(count ? count : 1, sizeof(*d->fx_state_size));
    for (DWORD i = 0; i < count; i++) {
        DWORD sz = d->fx_desc->aEffectMaps[i].dwStateSize * 4;
        if (sz > (1u << 20)) sz = 1u << 20;
        d->fx_state[i] = calloc(1, sz ? sz : 4);
        d->fx_state_size[i] = sz;
    }
    if (loc) d->fx_loc = *loc;
    if (ppDesc) *ppDesc = d->fx_desc;
    xlog("DSound: effects image accepted (%u bytes, %u effects%s)", size, count, src ? "" : ", descriptor not found");
    return DS_OK;
}

static HRESULT NTAPI DS_DownloadEffectsImage(void *self, const void *pv, DWORD size, const DSEFFECTIMAGELOC *loc,
                                             DSEFFECTIMAGEDESC **ppDesc)
{
    LOCK();
    struct ds_object *d = find_ds(self);
    HRESULT hr = d ? fx_download_locked(d, pv, size, loc, ppDesc) : DSERR_INVALIDPARAM;
    UNLOCK();
    return hr;
}

static HRESULT NTAPI DS_GetEffectData(void *self, DWORD index, DWORD offset, void *pv, DWORD size)
{
    LOCK();
    struct ds_object *d = find_ds(self);
    HRESULT hr = DSERR_INVALIDPARAM;
    if (d && pv && index < d->fx_count && offset <= d->fx_state_size[index] &&
        size <= d->fx_state_size[index] - offset) {
        memcpy(pv, d->fx_state[index] + offset, size);
        hr = DS_OK;
    }
    UNLOCK();
    return hr;
}

static HRESULT NTAPI DS_SetEffectData(void *self, DWORD index, DWORD offset, const void *pv, DWORD size, DWORD apply)
{
    (void)apply;
    LOCK();
    struct ds_object *d = find_ds(self);
    HRESULT hr = DSERR_INVALIDPARAM;
    if (d && pv && index < d->fx_count && offset <= d->fx_state_size[index] &&
        size <= d->fx_state_size[index] - offset) {
        memcpy(d->fx_state[index] + offset, pv, size);
        hr = DS_OK;
    }
    UNLOCK();
    return hr;
}

static HRESULT NTAPI DS_CommitEffectData(void *self)
{
    LOCK();
    struct ds_object *d = find_ds(self);
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

/* ---- files (host side) -------------------------------------------------------------------- */

struct ds_xfile { int fd; HANDLE h; bool own; };

static int xfile_open(struct ds_xfile *f, const char *name, int flags)
{
    f->fd = -1; f->h = NULL; f->own = false;
    if (!name) return -1;
    OBJECT_STRING s = { (USHORT)strlen(name), (USHORT)(strlen(name) + 1), (char *)name };
    OBJECT_ATTRIBUTES oa = { NULL, &s, 0 };
    char host[1024]; int is_dev;
    NTSTATUS st = fs_translate(&oa, host, sizeof(host), &is_dev);
    if (!NT_SUCCESS(st)) { xlog("DSound: cannot resolve %s (%#x)", name, st); return -1; }
    if (is_dev == 3) {
        /* On an Xbox disc (kernel/dvd.c): read it through a handle. */
        extern NTSTATUS NTAPI NtOpenFile(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *, ULONG, ULONG);
        IO_STATUS_BLOCK iosb;
        st = NtOpenFile(&f->h, 0x80000000 /* GENERIC_READ */, &oa, &iosb, 1, 0x60 /* sync, non-directory */);
        if (!NT_SUCCESS(st)) { f->h = NULL; xlog("DSound: cannot open %s (%#x)", name, st); return -1; }
        f->own = true;
        return 0;
    }
    f->fd = open(host, flags, 0644);
    if (f->fd < 0) { xlog("DSound: cannot open %s (%s)", name, host); return -1; }
    return 0;
}

static void xfile_close(struct ds_xfile *f)
{
    if (f->fd >= 0) close(f->fd);
    if (f->own && f->h) handle_close(f->h);
    f->fd = -1; f->h = NULL; f->own = false;
}

static ssize_t xfile_pread(struct ds_xfile *f, void *buf, size_t n, uint64_t off)
{
    if (f->fd >= 0) return pread(f->fd, buf, n, (off_t)off);
    extern NTSTATUS NTAPI NtReadFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG, LARGE_INTEGER *);
    IO_STATUS_BLOCK iosb = { { 0 }, 0 };
    LARGE_INTEGER o; o.QuadPart = (LONGLONG)off;
    NTSTATUS st = NtReadFile(f->h, NULL, NULL, NULL, &iosb, buf, (ULONG)n, &o);
    if (st == STATUS_END_OF_FILE) return 0;
    if (!NT_SUCCESS(st)) return -1;
    return (ssize_t)iosb.Information;
}

static ssize_t xfile_pwrite(struct ds_xfile *f, const void *buf, size_t n, uint64_t off)
{
    if (f->fd >= 0) return pwrite(f->fd, buf, n, (off_t)off);
    extern NTSTATUS NTAPI NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG, LARGE_INTEGER *);
    IO_STATUS_BLOCK iosb = { { 0 }, 0 };
    LARGE_INTEGER o; o.QuadPart = (LONGLONG)off;
    NTSTATUS st = NtWriteFile(f->h, NULL, NULL, NULL, &iosb, (PVOID)buf, (ULONG)n, &o);
    if (!NT_SUCCESS(st)) return -1;
    return (ssize_t)iosb.Information;
}

static uint64_t xfile_size(struct ds_xfile *f)
{
    if (f->fd >= 0) { struct stat sb; return fstat(f->fd, &sb) == 0 ? (uint64_t)sb.st_size : 0; }
    extern NTSTATUS NTAPI NtQueryInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, ULONG);
    struct { LARGE_INTEGER AllocationSize, EndOfFile; ULONG NumberOfLinks; BOOLEAN DeletePending, Directory; } info;
    IO_STATUS_BLOCK iosb = { { 0 }, 0 };
    if (!NT_SUCCESS(NtQueryInformationFile(f->h, &iosb, &info, sizeof(info), 5))) return 0;
    return (uint64_t)info.EndOfFile.QuadPart;
}

/* ---- XMediaObjects: wave file and raw file ---------------------------------------------- */

#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

struct ds_wavexmo {
    void **vtbl;
    uint32_t refs;
    struct ds_xfile f;
    uint8_t fmt[64]; DWORD fmt_size;      /* the WAVEFORMATEX the app sees through GetFormat */
    DWORD data_off, data_size, read_off;
    int has_loop; DWORD loop_start, loop_len;
};

struct ds_filexmo {
    void **vtbl;
    uint32_t refs;
    struct ds_xfile f;
    uint64_t pos;
};

static void *wave_vtbl[11];
static void *file_vtbl[9];

static void xmo_accept(const XMEDIAPACKET *p)
{
    if (p->pdwCompletedSize) *p->pdwCompletedSize = 0;
    if (p->pdwStatus) *p->pdwStatus = XMEDIAPACKET_STATUS_PENDING;
}

static void xmo_complete(const XMEDIAPACKET *p, DWORD size, DWORD status)
{
    extern NTSTATUS NTAPI NtSetEvent(HANDLE, LONG *);
    if (p->pdwCompletedSize) *p->pdwCompletedSize = size;
    if (p->pdwStatus) *p->pdwStatus = status;
    if (p->pContext) NtSetEvent((HANDLE)p->pContext, NULL);
}

static HRESULT wave_parse(struct ds_wavexmo *w)
{
    uint8_t hdr[12];
    if (xfile_pread(&w->f, hdr, 12, 0) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        xlog("DSound: not a RIFF WAVE file");
        return DSERR_INVALIDPARAM;
    }
    uint32_t riff_size; memcpy(&riff_size, hdr + 4, 4);
    uint64_t end = 8 + (uint64_t)riff_size, pos = 12;
    uint64_t fsize = xfile_size(&w->f);
    if (end > fsize) end = fsize;
    int have_fmt = 0, have_data = 0;
    while (pos + 8 <= end) {
        uint8_t ch[8];
        if (xfile_pread(&w->f, ch, 8, pos) != 8) break;
        uint32_t csize; memcpy(&csize, ch + 4, 4);
        uint64_t body = pos + 8;
        if (!memcmp(ch, "fmt ", 4) && !have_fmt) {
            DWORD n = csize > sizeof(w->fmt) ? sizeof(w->fmt) : csize;
            if (xfile_pread(&w->f, w->fmt, n, body) != (ssize_t)n) return DSERR_INVALIDPARAM;
            if (n < 16) return DSERR_INVALIDPARAM;
            if (n < 18) { w->fmt[16] = w->fmt[17] = 0; n = 18; }
            w->fmt_size = n;
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4) && !have_data) {
            w->data_off = (DWORD)body;
            w->data_size = csize;
            if (body + csize > fsize) w->data_size = (DWORD)(fsize - body);
            have_data = 1;
        } else if (!memcmp(ch, "wsmp", 4) && !w->has_loop) {
            /* WAVESAMPLE (20 bytes) followed by WAVESAMPLE_LOOP entries (16 bytes). */
            uint8_t ws[20];
            if (csize >= 20 && xfile_pread(&w->f, ws, 20, body) == 20) {
                uint32_t loops; memcpy(&loops, ws + 16, 4);
                for (uint32_t i = 0; i < loops && i < 64; i++) {
                    uint8_t lp[16];
                    if (xfile_pread(&w->f, lp, 16, body + 20 + i * 16) != 16) break;
                    uint32_t type, start, len;
                    memcpy(&type, lp + 4, 4); memcpy(&start, lp + 8, 4); memcpy(&len, lp + 12, 4);
                    if (type == 0 || type == 1) { w->has_loop = 1; w->loop_start = start; w->loop_len = len; break; }
                }
            }
        }
        pos = body + csize + (csize & 1);
    }
    if (!have_fmt || !have_data) { xlog("DSound: WAVE file without fmt/data chunk"); return DSERR_INVALIDPARAM; }
    const WAVEFORMATEX *f = (const WAVEFORMATEX *)w->fmt;
    if (w->has_loop) {
        if (f->wFormatTag == WAVE_FORMAT_XBOX_ADPCM) {
            w->loop_start = w->loop_start / (f->nChannels * 64) * (f->nChannels * 36);
            w->loop_len = w->loop_len / (f->nChannels * 64) * (f->nChannels * 36);
        } else {
            w->loop_start *= f->nBlockAlign;
            w->loop_len *= f->nBlockAlign;
        }
    }
    return DS_OK;
}

static ULONG NTAPI Wave_AddRef(struct ds_wavexmo *w) { return ++w->refs; }
static ULONG NTAPI Wave_Release(struct ds_wavexmo *w)
{
    if (--w->refs) return w->refs;
    xfile_close(&w->f);
    free(w);
    return 0;
}
static HRESULT NTAPI Wave_GetInfo(struct ds_wavexmo *w, XMEDIAINFO *info)
{
    if (!info) return DSERR_INVALIDPARAM;
    info->dwFlags = XMO_STREAMF_FIXED_SAMPLE_SIZE;
    info->dwInputSize = 0;
    info->dwOutputSize = ((const WAVEFORMATEX *)w->fmt)->nBlockAlign;
    info->dwMaxLookahead = 0;
    return DS_OK;
}
static HRESULT NTAPI Wave_GetStatus(struct ds_wavexmo *w, DWORD *st)
{
    (void)w;
    if (!st) return DSERR_INVALIDPARAM;
    *st = XMO_STATUSF_ACCEPT_OUTPUT_DATA;
    return DS_OK;
}
static HRESULT NTAPI Wave_Process(struct ds_wavexmo *w, const XMEDIAPACKET *src, const XMEDIAPACKET *dst)
{
    (void)src;
    if (!dst || !dst->pvBuffer) return DSERR_INVALIDPARAM;
    DWORD align = ((const WAVEFORMATEX *)w->fmt)->nBlockAlign;
    DWORD n = align ? dst->dwMaxSize / align * align : dst->dwMaxSize;
    xmo_accept(dst);
    if (w->read_off >= w->data_size) n = 0;
    else if (n > w->data_size - w->read_off) n = w->data_size - w->read_off;
    ssize_t got = n ? xfile_pread(&w->f, dst->pvBuffer, n, (uint64_t)w->data_off + w->read_off) : 0;
    if (got < 0) { xmo_complete(dst, 0, XMEDIAPACKET_STATUS_FAILURE); return DSERR_GENERIC; }
    w->read_off += (DWORD)got;
    xmo_complete(dst, (DWORD)got, XMEDIAPACKET_STATUS_SUCCESS);
    return DS_OK;
}
static HRESULT NTAPI Wave_Discontinuity(struct ds_wavexmo *w) { (void)w; return DS_OK; }
static HRESULT NTAPI Wave_Seek(struct ds_wavexmo *w, LONG off, DWORD origin, DWORD *abs_)
{
    if (origin == FILE_BEGIN) w->read_off = 0;
    else if (origin == FILE_END) w->read_off = w->data_size;
    w->read_off += (DWORD)off;
    if (abs_) *abs_ = w->read_off;
    return DS_OK;
}
static HRESULT NTAPI Wave_Flush(struct ds_wavexmo *w) { return Wave_Seek(w, 0, FILE_BEGIN, NULL); }
static HRESULT NTAPI Wave_GetLength(struct ds_wavexmo *w, DWORD *len)
{
    if (!len) return DSERR_INVALIDPARAM;
    *len = w->data_size;
    return DS_OK;
}
static HRESULT NTAPI Wave_GetFormat(struct ds_wavexmo *w, const WAVEFORMATEX **pp)
{
    if (!pp) return DSERR_INVALIDPARAM;
    *pp = (const WAVEFORMATEX *)w->fmt;
    return DS_OK;
}
static HRESULT NTAPI Wave_GetLoopRegion(struct ds_wavexmo *w, DWORD *start, DWORD *len)
{
    if (!w->has_loop) return DSERR_GENERIC;
    if (start) *start = w->loop_start;
    if (len) *len = w->loop_len;
    return DS_OK;
}

static HRESULT wave_create(const char *name, HANDLE h, const WAVEFORMATEX **ppfmt, void **ppxmo)
{
    if (!ppxmo || (!name && !h) || (name && h)) return DSERR_INVALIDPARAM;
    struct ds_wavexmo *w = calloc(1, sizeof(*w));
    if (!w) return DSERR_OUTOFMEMORY;
    guest_vtbls();
    w->vtbl = wave_vtbl; w->refs = 1;
    if (name) { if (xfile_open(&w->f, name, O_RDONLY) < 0) { free(w); return DSERR_INVALIDPARAM; } }
    else { w->f.fd = -1; w->f.h = h; w->f.own = false; }
    HRESULT hr = wave_parse(w);
    if (hr != DS_OK) { xfile_close(&w->f); free(w); return hr; }
    if (ppfmt) *ppfmt = (const WAVEFORMATEX *)w->fmt;
    *ppxmo = w;
    const WAVEFORMATEX *f = (const WAVEFORMATEX *)w->fmt;
    xlog("DSound: wave media object %s: tag %#x, %u ch, %u Hz, %u data bytes%s", name ? name : "(handle)",
         f->wFormatTag, f->nChannels, f->nSamplesPerSec, w->data_size, w->has_loop ? ", loop" : "");
    return DS_OK;
}

static HRESULT NTAPI XWaveFileCreateMediaObject(const char *name, const WAVEFORMATEX **ppfmt, void **ppxmo)
{
    if (!name) return DSERR_INVALIDPARAM;
    return wave_create(name, NULL, ppfmt, ppxmo);
}

static HRESULT NTAPI XWaveFileCreateMediaObjectEx(const char *name, HANDLE h, void **ppxmo)
{
    return wave_create(name, h, NULL, ppxmo);
}

static ULONG NTAPI File_AddRef(struct ds_filexmo *f) { return ++f->refs; }
static ULONG NTAPI File_Release(struct ds_filexmo *f)
{
    if (--f->refs) return f->refs;
    xfile_close(&f->f);
    free(f);
    return 0;
}
static HRESULT NTAPI File_GetInfo(struct ds_filexmo *f, XMEDIAINFO *info)
{
    (void)f;
    if (!info) return DSERR_INVALIDPARAM;
    info->dwFlags = XMO_STREAMF_FIXED_SAMPLE_SIZE;
    info->dwInputSize = 1; info->dwOutputSize = 1; info->dwMaxLookahead = 0;
    return DS_OK;
}
static HRESULT NTAPI File_GetStatus(struct ds_filexmo *f, DWORD *st)
{
    (void)f;
    if (!st) return DSERR_INVALIDPARAM;
    *st = XMO_STATUSF_ACCEPT_INPUT_DATA | XMO_STATUSF_ACCEPT_OUTPUT_DATA;
    return DS_OK;
}
static HRESULT NTAPI File_Process(struct ds_filexmo *f, const XMEDIAPACKET *src, const XMEDIAPACKET *dst)
{
    const XMEDIAPACKET *p = src ? src : dst;
    if (!p || (src && dst) || !p->pvBuffer) return DSERR_INVALIDPARAM;
    xmo_accept(p);
    ssize_t n = src ? xfile_pwrite(&f->f, p->pvBuffer, p->dwMaxSize, f->pos)
                    : xfile_pread(&f->f, p->pvBuffer, p->dwMaxSize, f->pos);
    if (n < 0) { xmo_complete(p, 0, XMEDIAPACKET_STATUS_FAILURE); return DSERR_GENERIC; }
    f->pos += (uint64_t)n;
    xmo_complete(p, (DWORD)n, XMEDIAPACKET_STATUS_SUCCESS);
    return DS_OK;
}
static HRESULT NTAPI File_Discontinuity(struct ds_filexmo *f) { (void)f; return DS_OK; }
static HRESULT NTAPI File_Seek(struct ds_filexmo *f, LONG off, DWORD origin, DWORD *abs_)
{
    if (origin == FILE_BEGIN) f->pos = 0;
    else if (origin == FILE_END) f->pos = xfile_size(&f->f);
    f->pos = (uint64_t)((int64_t)f->pos + off);
    if (abs_) *abs_ = (DWORD)f->pos;
    return DS_OK;
}
static HRESULT NTAPI File_Flush(struct ds_filexmo *f) { return File_Seek(f, 0, FILE_BEGIN, NULL); }
static HRESULT NTAPI File_GetLength(struct ds_filexmo *f, DWORD *len)
{
    if (!len) return DSERR_INVALIDPARAM;
    *len = (DWORD)xfile_size(&f->f);
    return DS_OK;
}

static HRESULT NTAPI XFileCreateMediaObject(const char *name, DWORD access, DWORD share, DWORD disp, DWORD attrs,
                                            void **ppxmo)
{
    (void)share; (void)attrs;
    if (!name || !ppxmo) return DSERR_INVALIDPARAM;
    int flags = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: flags |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: flags |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 4: flags |= O_CREAT; break;            /* OPEN_ALWAYS */
    case 5: flags |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    default: break;                             /* OPEN_EXISTING */
    }
    struct ds_filexmo *f = calloc(1, sizeof(*f));
    if (!f) return DSERR_OUTOFMEMORY;
    guest_vtbls();
    f->vtbl = file_vtbl; f->refs = 1;
    if (xfile_open(&f->f, name, flags) < 0) { free(f); return DSERR_INVALIDPARAM; }
    *ppxmo = f;
    return DS_OK;
}

static HRESULT NTAPI XFileCreateMediaObjectEx(HANDLE h, void **ppxmo)
{
    if (!h || !ppxmo) return DSERR_INVALIDPARAM;
    struct ds_filexmo *f = calloc(1, sizeof(*f));
    if (!f) return DSERR_OUTOFMEMORY;
    guest_vtbls();
    f->vtbl = file_vtbl; f->refs = 1; f->f.fd = -1; f->f.h = h; f->f.own = false;
    *ppxmo = f;
    return DS_OK;
}

static HRESULT NTAPI Ac97CreateMediaObject(DWORD channel, void *cb, void *ctx, void **pp)
{
    (void)channel; (void)cb; (void)ctx;
    if (pp) *pp = NULL;
    xlog("DSound: Ac97CreateMediaObject is not supported");
    return DSERR_UNSUPPORTED;
}

static HRESULT NTAPI XAudioDownloadEffectsImage(const char *name, const DSEFFECTIMAGELOC *loc, DWORD flags,
                                                DSEFFECTIMAGEDESC **ppDesc)
{
    if (!name || (flags & ~1u)) return DSERR_INVALIDPARAM;
    void *data = NULL; DWORD size = 0;
    if (flags & 1) {
        /* XAUDIO_DOWNLOADFX_XBESECTION: the image is a section of the XBE. */
        XBE_HEADER *h = (XBE_HEADER *)0x10000;
        XBE_SECTION *sec = (XBE_SECTION *)h->SectionHeaders;
        for (ULONG i = 0; i < h->NumberOfSections; i++) {
            if (!strcmp((const char *)sec[i].SectionName, name)) {
                data = (void *)sec[i].VirtualAddress;
                size = sec[i].SizeOfRawData;
                break;
            }
        }
        if (!data) { xlog("DSound: XBE section %s not found", name); return DSERR_INVALIDPARAM; }
    } else {
        struct ds_xfile f;
        if (xfile_open(&f, name, O_RDONLY) < 0) return DSERR_INVALIDPARAM;
        size = (DWORD)xfile_size(&f);
        data = malloc(size ? size : 1);
        if (xfile_pread(&f, data, size, 0) != (ssize_t)size) { xfile_close(&f); free(data); return DSERR_GENERIC; }
        xfile_close(&f);
    }
    LOCK();
    HRESULT hr = g.ds ? fx_download_locked(g.ds, data, size, loc, ppDesc) : DSERR_INVALIDCALL;
    UNLOCK();
    if (!(flags & 1)) free(data);
    return hr;
}

/* ---- IDirectSound methods ----------------------------------------------------------------- */

static HRESULT NTAPI DS_GetCaps(void *self, DSCAPS *caps)
{
    if (!caps) return DSERR_INVALIDPARAM;
    LOCK();
    struct ds_object *d = find_ds(self);
    DWORD hw = 0, hw3d = 0, sges = 0;
    for (unsigned i = 0; i < g.nobjs; i++) {
        struct ds_voice *v = NULL; DWORD bytes = 0;
        if (g.objs[i].kind == DS_BUFFER) { v = &((struct ds_buffer *)g.objs[i].obj)->v; bytes = ((struct ds_buffer *)g.objs[i].obj)->size; }
        else if (g.objs[i].kind == DS_STREAM) v = &((struct ds_stream *)g.objs[i].obj)->v;
        if (!v) continue;
        DWORD n = ((v->fmt.ch ? v->fmt.ch : 1) - 1) / 2 + 1;
        hw += n;
        if (v->flags & DSBCAPS_CTRL3D) hw3d += n;
        sges += (bytes + 4095) / 4096;
    }
    caps->dwFree2DBuffers = hw < 256 ? 256 - hw : 0;
    caps->dwFree3DBuffers = hw3d < 64 ? 64 - hw3d : 0;
    caps->dwFreeBufferSGEs = sges < 2048 ? 2048 - sges : 0;
    caps->dwMemoryAllocated = g.mem_allocated;
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_CreateSoundBuffer(void *self, const DSBUFFERDESC *desc, uint32_t *pp, void *unk)
{
    if (unk) return DSERR_INVALIDPARAM;
    LOCK();
    HRESULT hr = find_ds(self) ? buffer_create_locked(desc, pp) : DSERR_INVALIDPARAM;
    UNLOCK();
    return hr;
}

static HRESULT NTAPI DS_CreateSoundStream(void *self, const DSSTREAMDESC *desc, uint32_t *pp, void *unk)
{
    if (unk) return DSERR_INVALIDPARAM;
    LOCK();
    HRESULT hr = find_ds(self) ? stream_create_locked(desc, pp) : DSERR_INVALIDPARAM;
    UNLOCK();
    return hr;
}

static HRESULT NTAPI DS_GetSpeakerConfig(void *self, DWORD *cfg)
{
    if (!cfg) return DSERR_INVALIDPARAM;
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) *cfg = d->speaker_config & 0x7FFFFFFFu;
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_SetCooperativeLevel(void *self, void *hwnd, DWORD level)
{
    (void)self; (void)hwnd; (void)level;
    return DS_OK;
}

static HRESULT NTAPI DS_Compact(void *self) { (void)self; return DS_OK; }

static HRESULT NTAPI DS_EnableHeadphones(void *self, ULONG on)
{
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) { d->headphones = !!on; mark_all_3d_dirty(); }
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_SetMixBinHeadroom(void *self, DWORD bin, DWORD headroom)
{
    if (bin >= DSMIXBIN_COUNT || headroom > 7) return DSERR_INVALIDPARAM;
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) {
        d->mixbin_headroom[bin] = (uint8_t)headroom;
        for (unsigned i = 0; i < g.nobjs; i++) {
            if (g.objs[i].kind == DS_BUFFER) ((struct ds_buffer *)g.objs[i].obj)->v.dirty = 1;
            else if (g.objs[i].kind == DS_STREAM) ((struct ds_stream *)g.objs[i].obj)->v.dirty = 1;
        }
    }
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

/* Listener 3D settings: immediate values go to both copies, deferred ones to the shadow. */
#define LISTENER_SET(d, apply, ...) do { \
        struct ds_listener *L = &(d)->listener_def; __VA_ARGS__; \
        if ((apply) & DS3D_DEFERRED) (d)->ldef_dirty = 1; \
        else { L = &(d)->listener; __VA_ARGS__; mark_all_3d_dirty(); } \
    } while (0)

static HRESULT NTAPI DS_SetAllParameters(void *self, const DS3DLISTENER *p, DWORD apply)
{
    if (!p || p->dwSize != sizeof(*p)) return DSERR_INVALIDPARAM;
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) LISTENER_SET(d, apply, {
        memcpy(L->pos, p->vPosition, 12); memcpy(L->vel, p->vVelocity, 12);
        memcpy(L->front, p->vOrientFront, 12); memcpy(L->top, p->vOrientTop, 12);
        L->dist_factor = p->flDistanceFactor; L->rolloff = p->flRolloffFactor; L->doppler = p->flDopplerFactor; });
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_SetOrientation(void *self, float xf, float yf, float zf, float xt, float yt, float zt, DWORD apply)
{
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) LISTENER_SET(d, apply, { L->front[0] = xf; L->front[1] = yf; L->front[2] = zf;
                                    L->top[0] = xt; L->top[1] = yt; L->top[2] = zt; });
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_SetPosition(void *self, float x, float y, float z, DWORD apply)
{
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) LISTENER_SET(d, apply, { L->pos[0] = x; L->pos[1] = y; L->pos[2] = z; });
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_SetVelocity(void *self, float x, float y, float z, DWORD apply)
{
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) LISTENER_SET(d, apply, { L->vel[0] = x; L->vel[1] = y; L->vel[2] = z; });
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_SetDistanceFactor(void *self, float f, DWORD apply)
{
    if (!(f > 0)) return DSERR_INVALIDPARAM;
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) LISTENER_SET(d, apply, { L->dist_factor = f; });
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_SetDopplerFactor(void *self, float f, DWORD apply)
{
    if (!(f >= 0 && f <= 10)) return DSERR_INVALIDPARAM;
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) LISTENER_SET(d, apply, { L->doppler = f; });
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_SetRolloffFactor(void *self, float f, DWORD apply)
{
    if (!(f >= 0 && f <= 10)) return DSERR_INVALIDPARAM;
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) LISTENER_SET(d, apply, { L->rolloff = f; });
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_SetI3DL2Listener(void *self, const void *p, DWORD apply)
{
    (void)p; (void)apply;
    LOCK();
    struct ds_object *d = find_ds(self);
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static void voice_commit_locked(struct ds_voice *v)
{
    if (v->def_dirty) { v->p3d = v->p3d_def; v->def_dirty = 0; v->dirty = 1; }
}

static HRESULT NTAPI DS_CommitDeferredSettings(void *self)
{
    LOCK();
    struct ds_object *d = find_ds(self);
    if (d) {
        if (d->ldef_dirty) { d->listener = d->listener_def; d->ldef_dirty = 0; }
        for (unsigned i = 0; i < g.nobjs; i++) {
            if (g.objs[i].kind == DS_BUFFER) voice_commit_locked(&((struct ds_buffer *)g.objs[i].obj)->v);
            else if (g.objs[i].kind == DS_STREAM) voice_commit_locked(&((struct ds_stream *)g.objs[i].obj)->v);
        }
        mark_all_3d_dirty();
    }
    UNLOCK();
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

static HRESULT NTAPI DS_GetTime(void *self, int64_t *prt)
{
    if (!prt) return DSERR_INVALIDPARAM;
    LOCK();
    struct ds_object *d = find_ds(self);
    int64_t t = reference_time_locked();
    UNLOCK();
    memcpy(prt, &t, 8);
    return d ? DS_OK : DSERR_INVALIDPARAM;
}

/* ---- voice settings (buffers and streams) ------------------------------------------------- */

#define VOICE_ENTRY(v, self) \
    struct ds_voice *v; LOCK(); v = find_voice(self); \
    if (!v) { UNLOCK(); xlog("DSound: %s on unknown object %p", __func__ + 6, self); return DSERR_INVALIDPARAM; }
#define VOICE_RETURN(hr) do { HRESULT hr_ = (hr); UNLOCK(); return hr_; } while (0)

static HRESULT NTAPI Voice_SetFormat(void *self, const WAVEFORMATEX *w)
{
    struct ds_fmt fmt; DWORD mask = 0;
    HRESULT hr = fmt_parse(w, &fmt, &mask);
    if (hr != DS_OK) return hr;
    VOICE_ENTRY(v, self);
    if (v->flags & DSBCAPS_SUBMIXMASK) VOICE_RETURN(DSERR_INVALIDCALL);
    v->fmt = fmt;
    v->pitch = calc_pitch(fmt.rate);
    /* Only a format with a channel mask reassigns the mixbins of a 2D voice;
       otherwise they stay as they are (CDirectSoundVoiceSettings::SetFormat). */
    if (mask && !(v->flags & DSBCAPS_CTRL3D)) { voice_mask_bins(v, mask); voice_keep_submix_bin(v); }
    v->adpcm_block = -1;
    v->dirty = 1;
    if (v->kind == DS_BUFFER) {
        struct ds_buffer *b = v->owner;
        b->pos = 0; b->cursor = 0;
    }
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Voice_SetFrequency(void *self, DWORD freq)
{
    if (freq && (freq < DSBFREQUENCY_MIN || freq > DSBFREQUENCY_MAX)) return DSERR_INVALIDPARAM;
    VOICE_ENTRY(v, self);
    v->pitch = calc_pitch(freq ? freq : v->fmt.rate);
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Voice_SetVolume(void *self, LONG vol)
{
    if (vol < DSBVOLUME_MIN || vol > 0) return DSERR_INVALIDPARAM;
    VOICE_ENTRY(v, self);
    v->volume = vol - (LONG)v->headroom;
    v->dirty = 1;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Voice_SetPitch(void *self, LONG pitch)
{
    if (pitch < DSBPITCH_MIN || pitch > DSBPITCH_MAX) return DSERR_INVALIDPARAM;
    VOICE_ENTRY(v, self);
    v->pitch = pitch;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Voice_SetLFO(void *self, const void *desc)
{
    if (!desc) return DSERR_INVALIDPARAM;
    VOICE_ENTRY(v, self);
    VOICE_RETURN(DS_OK);
}

/* New envelope registers; a running segment keeps going and picks up new
   lengths at its next transition, as the APU does. */
static HRESULT NTAPI Voice_SetEG(void *self, const void *desc)
{
    if (!desc) return DSERR_INVALIDPARAM;
    const DWORD *d = desc;
    if (d[0] > 1) return DSERR_INVALIDPARAM;
    VOICE_ENTRY(v, self);
    struct ds_eg *e = &v->eg[d[0] == 1 ? DSEG_IDX_AMP : DSEG_IDX_MULTI];
    memcpy(e->d, d, sizeof(e->d));
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Voice_SetFilter(void *self, const void *desc)
{
    if (!desc) return DSERR_INVALIDPARAM;
    const DWORD *d = desc;   /* dwMode, dwQCoefficient, adwCoefficients[4] */
    VOICE_ENTRY(v, self);
    if (v->fmode != 1 && (d[0] & 3) == 1) {   /* switched on: track from the new values */
        v->fc_cur = s16field(d[2]);
        v->q_cur = (d[3] & 0xFFFF) / 32768.0f;
    }
    v->fmode = d[0] & 3;
    v->fc0 = d[2] & 0xFFFF;
    v->fc1 = d[3] & 0xFFFF;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Voice_SetHeadroom(void *self, DWORD headroom)
{
    if (headroom > DSBHEADROOM_MAX) return DSERR_INVALIDPARAM;
    VOICE_ENTRY(v, self);
    v->volume += (LONG)v->headroom - (LONG)headroom;
    v->headroom = headroom;
    v->dirty = 1;
    VOICE_RETURN(DS_OK);
}

/* SetOutputBuffer (CDirectSoundVoiceSettings::SetOutputBuffer): the voice
   is routed to the submix buffer's input bin *instead of* its mixbins (the
   mixbin list becomes that one bin), and keeps a reference on the buffer.
   Disconnecting drops that bin, leaving whatever came before it. */
static HRESULT NTAPI Voice_SetOutputBuffer(void *self, void *out)
{
    struct ds_events ev = { .n = 0 };
    VOICE_ENTRY(v, self);
    struct ds_buffer *ob = out ? find_buffer(out) : NULL;
    if (out && (!ob || !(ob->v.flags & DSBCAPS_SUBMIXMASK) || &ob->v == v)) VOICE_RETURN(DSERR_INVALIDPARAM);
    if (ob == v->output) VOICE_RETURN(DS_OK);
    if (v->output) {
        struct ds_buffer *old = v->output;
        if (v->nbins && v->bins[v->nbins - 1] == (uint8_t)old->input_mixbin) v->nbins--;
        v->output = NULL;
        buffer_release_locked(old, &ev);
    }
    if (ob) {
        ob->hdr.refs++;
        v->output = ob;
        v->nbins = 1;
        v->bins[0] = (uint8_t)ob->input_mixbin;
    }
    v->dirty = 1;
    UNLOCK();
    events_signal(&ev);
    return DS_OK;
}

static HRESULT NTAPI Voice_SetMixBins(void *self, const DSMIXBINS *p)
{
    VOICE_ENTRY(v, self);
    VOICE_RETURN(voice_set_mixbins(v, p));
}

static HRESULT NTAPI Voice_SetMixBinVolumes(void *self, const DSMIXBINS *p)
{
    if (!p || (p->dwMixBinCount && !p->lpMixBinVolumePairs)) return DSERR_INVALIDPARAM;
    VOICE_ENTRY(v, self);
    for (DWORD i = 0; i < p->dwMixBinCount; i++) {
        DWORD b = p->lpMixBinVolumePairs[i].dwMixBin; LONG vol = p->lpMixBinVolumePairs[i].lVolume;
        if (b >= DSMIXBIN_COUNT || vol < DSBVOLUME_MIN || vol > 0) VOICE_RETURN(DSERR_INVALIDPARAM);
        DWORD k;
        for (k = 0; k < v->nbins; k++) if (v->bins[k] == b) break;
        if (k == v->nbins) VOICE_RETURN(DSERR_INVALIDPARAM);
        v->binvol[b] = vol;
    }
    v->dirty = 1;
    VOICE_RETURN(DS_OK);
}

/* 3D setters: immediate values go to both copies, deferred ones to the shadow. */
#define VOICE_3D(self, apply, ...) \
    VOICE_ENTRY(v, self); \
    if (!(v->flags & DSBCAPS_CTRL3D)) VOICE_RETURN(DSERR_CONTROLUNAVAIL); \
    { struct ds_3d *P = &v->p3d_def; __VA_ARGS__; \
      if ((apply) & DS3D_DEFERRED) v->def_dirty = 1; else { P = &v->p3d; __VA_ARGS__; v->dirty = 1; } } \
    VOICE_RETURN(DS_OK)

static HRESULT NTAPI Voice_SetAllParameters(void *self, const DS3DBUFFER *p, DWORD apply)
{
    if (!p || p->dwSize != sizeof(*p)) return DSERR_INVALIDPARAM;
    VOICE_3D(self, apply, {
        memcpy(P->pos, p->vPosition, 12); memcpy(P->vel, p->vVelocity, 12);
        P->cone_in = p->dwInsideConeAngle; P->cone_out = p->dwOutsideConeAngle;
        memcpy(P->cone_dir, p->vConeOrientation, 12); P->cone_vol = p->lConeOutsideVolume;
        P->min_dist = p->flMinDistance; P->max_dist = p->flMaxDistance; P->mode = p->dwMode;
        P->dist_factor = p->flDistanceFactor; P->rolloff = p->flRolloffFactor; P->doppler = p->flDopplerFactor; });
}

static HRESULT NTAPI Voice_SetConeAngles(void *self, DWORD in, DWORD out, DWORD apply)
{
    if (in > 360 || out > 360 || in > out) return DSERR_INVALIDPARAM;
    VOICE_3D(self, apply, { P->cone_in = in; P->cone_out = out; });
}

static HRESULT NTAPI Voice_SetConeOrientation(void *self, float x, float y, float z, DWORD apply)
{
    VOICE_3D(self, apply, { P->cone_dir[0] = x; P->cone_dir[1] = y; P->cone_dir[2] = z; });
}

static HRESULT NTAPI Voice_SetConeOutsideVolume(void *self, LONG vol, DWORD apply)
{
    if (vol < DSBVOLUME_MIN || vol > 0) return DSERR_INVALIDPARAM;
    VOICE_3D(self, apply, { P->cone_vol = vol; });
}

static HRESULT NTAPI Voice_SetMaxDistance(void *self, float f, DWORD apply)
{
    if (!(f > 0)) return DSERR_INVALIDPARAM;
    VOICE_3D(self, apply, { P->max_dist = f; });
}

static HRESULT NTAPI Voice_SetMinDistance(void *self, float f, DWORD apply)
{
    if (!(f > 0)) return DSERR_INVALIDPARAM;
    VOICE_3D(self, apply, { P->min_dist = f; });
}

static HRESULT NTAPI Voice_SetMode(void *self, DWORD mode, DWORD apply)
{
    if (mode > DS3DMODE_DISABLE) return DSERR_INVALIDPARAM;
    VOICE_3D(self, apply, { P->mode = mode; });
}

static HRESULT NTAPI Voice_SetPosition(void *self, float x, float y, float z, DWORD apply)
{
    VOICE_3D(self, apply, { P->pos[0] = x; P->pos[1] = y; P->pos[2] = z; });
}

static HRESULT NTAPI Voice_SetVelocity(void *self, float x, float y, float z, DWORD apply)
{
    VOICE_3D(self, apply, { P->vel[0] = x; P->vel[1] = y; P->vel[2] = z; });
}

static HRESULT NTAPI Voice_SetDistanceFactor(void *self, float f, DWORD apply)
{
    if (!(f > 0)) return DSERR_INVALIDPARAM;
    VOICE_3D(self, apply, { P->dist_factor = f; });
}

static HRESULT NTAPI Voice_SetDopplerFactor(void *self, float f, DWORD apply)
{
    if (!(f >= 0 && f <= 10)) return DSERR_INVALIDPARAM;
    VOICE_3D(self, apply, { P->doppler = f; });
}

static HRESULT NTAPI Voice_SetRolloffFactor(void *self, float f, DWORD apply)
{
    if (!(f >= 0 && f <= 10)) return DSERR_INVALIDPARAM;
    VOICE_3D(self, apply, { P->rolloff = f; });
}

static HRESULT NTAPI Voice_SetI3DL2Source(void *self, const void *p, DWORD apply)
{
    (void)apply;
    if (!p) return DSERR_INVALIDPARAM;
    VOICE_ENTRY(v, self);
    if (!(v->flags & DSBCAPS_CTRL3D)) VOICE_RETURN(DSERR_CONTROLUNAVAIL);
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Voice_CommitDeferredSettings(void *self)
{
    VOICE_ENTRY(v, self);
    voice_commit_locked(v);
    VOICE_RETURN(DS_OK);
}

/* ---- IDirectSoundBuffer methods ----------------------------------------------------------- */

#define BUFFER_ENTRY(b, self) \
    struct ds_buffer *b; LOCK(); b = find_buffer(self); \
    if (!b) { UNLOCK(); xlog("DSound: %s on unknown buffer %p", __func__ + 4, self); return DSERR_INVALIDPARAM; }

static HRESULT buffer_play_locked(struct ds_buffer *b, DWORD flags)
{
    if (b->v.flags & DSBCAPS_SUBMIXMASK) { b->playing = 1; return DS_OK; }
    if (!b->data || !b->play_len) { xlog("DSound: Play on buffer %p without data", b->iface); return DSERR_INVALIDCALL; }
    /* CMcpxBuffer::Play: a stopped voice resumes from the cached cursor
       unless FROMSTART; a playing one keeps its position unless FROMSTART;
       either way the cached cursor is consumed. */
    if (!b->playing || (flags & DSBPLAY_FROMSTART)) {
        DWORD start = (flags & DSBPLAY_FROMSTART) ? 0 : bytes_to_frames(&b->v.fmt, b->cursor);
        if (start >= bytes_to_frames(&b->v.fmt, b->play_len)) start = 0;
        b->pos = start;
        b->v.adpcm_block = -1;
        voice_on(&b->v);
    }
    b->cursor = 0;
    b->looping = !!(flags & DSBPLAY_LOOPING);
    if (!b->playing) g.plays++;
    b->playing = 1;
    if (b->nnotify) b->notify_active = 1;
    return DS_OK;
}

static HRESULT NTAPI Buf_Play(void *self, DWORD r1, DWORD r2, DWORD flags)
{
    if (r1 || r2 || (flags & ~(DSBPLAY_LOOPING | DSBPLAY_FROMSTART | DSBPLAY_SYNCHPLAYBACK))) return DSERR_INVALIDPARAM;
    BUFFER_ENTRY(b, self);
    b->start_at = 0;
    HRESULT hr = buffer_play_locked(b, flags);
    UNLOCK();
    return hr;
}

static HRESULT NTAPI Buf_PlayEx(void *self, uint32_t rt_lo, uint32_t rt_hi, DWORD flags)
{
    if (flags & ~(DSBPLAY_LOOPING | DSBPLAY_FROMSTART | DSBPLAY_SYNCHPLAYBACK)) return DSERR_INVALIDPARAM;
    int64_t rt = (int64_t)(((uint64_t)rt_hi << 32) | rt_lo);
    BUFFER_ENTRY(b, self);
    HRESULT hr = DS_OK;
    if (rt > 0 && rt > reference_time_locked() && b->data) {
        b->start_at = rt; b->start_flags = flags;
    } else {
        b->start_at = 0;
        hr = buffer_play_locked(b, flags);
    }
    UNLOCK();
    return hr;
}

static HRESULT NTAPI Buf_Stop(void *self)
{
    struct ds_events ev = { .n = 0 };
    BUFFER_ENTRY(b, self);
    buffer_stop_locked(b, &ev);
    UNLOCK();
    events_signal(&ev);
    return DS_OK;
}

static HRESULT NTAPI Buf_StopEx(void *self, uint32_t rt_lo, uint32_t rt_hi, DWORD flags)
{
    if (flags & ~(DSBSTOPEX_ENVELOPE | DSBSTOPEX_RELEASEWAVEFORM)) return DSERR_INVALIDPARAM;
    int64_t rt = (int64_t)(((uint64_t)rt_hi << 32) | rt_lo);
    struct ds_events ev = { .n = 0 };
    BUFFER_ENTRY(b, self);
    if (rt > 0 && rt > reference_time_locked() && b->playing) {
        b->stop_at = rt; b->stop_flags = flags;
    } else {
        b->stop_at = 0;
        buffer_stopex_locked(b, flags, &ev);
    }
    UNLOCK();
    events_signal(&ev);
    return DS_OK;
}

static HRESULT NTAPI Buf_SetPlayRegion(void *self, DWORD start, DWORD len)
{
    BUFFER_ENTRY(b, self);
    if (b->v.flags & DSBCAPS_SUBMIXMASK) VOICE_RETURN(DSERR_INVALIDCALL);
    if (start % 4 || start > b->size) VOICE_RETURN(DSERR_INVALIDPARAM);
    if (!len) len = b->size - start;
    else if (len % b->v.fmt.align) VOICE_RETURN(DSERR_INVALIDPARAM);
    if (start + len > b->size || start + len < start) VOICE_RETURN(DSERR_INVALIDCALL);
    b->play_start = start; b->play_len = len;
    b->loop_start = 0; b->loop_len = len;
    /* CMcpxBuffer::SetPlayRegion: drop the cached cursor; a playing voice
       restarts from the start of the new region, keeping its loop flag. */
    b->cursor = 0;
    if (b->playing) { b->pos = 0; b->v.adpcm_block = -1; }
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Buf_SetLoopRegion(void *self, DWORD start, DWORD len)
{
    BUFFER_ENTRY(b, self);
    if (b->v.flags & DSBCAPS_SUBMIXMASK) VOICE_RETURN(DSERR_INVALIDCALL);
    if (start % b->v.fmt.align || start > b->play_len) VOICE_RETURN(DSERR_INVALIDPARAM);
    if (!len) len = b->play_len - start;
    else if (len % b->v.fmt.align) VOICE_RETURN(DSERR_INVALIDPARAM);
    if (start + len > b->play_len || start + len < start) VOICE_RETURN(DSERR_INVALIDCALL);
    b->loop_start = start; b->loop_len = len;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Buf_GetStatus(void *self, DWORD *st)
{
    if (!st) return DSERR_INVALIDPARAM;
    BUFFER_ENTRY(b, self);
    DWORD s = 0;
    if (b->playing || b->start_at) s |= DSBSTATUS_PLAYING;
    if (b->playing && b->looping) s |= DSBSTATUS_LOOPING;
    if (b->paused) s |= DSBSTATUS_PAUSED;
    *st = s;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Buf_GetCurrentPosition(void *self, DWORD *play, DWORD *write)
{
    BUFFER_ENTRY(b, self);
    if (b->v.flags & DSBCAPS_SUBMIXMASK) VOICE_RETURN(DSERR_INVALIDCALL);
    const struct ds_fmt *f = &b->v.fmt;
    if (b->playing) {
        DWORD pc = frames_to_bytes(f, (DWORD)b->pos);
        if (play) *play = pc;
        if (write) {
            DWORD frame = frames_to_bytes(f, HW_FRAME);
            if (frame < f->align) frame = f->align;
            DWORD wc = pc + frame;
            if (b->looping && b->loop_len && pc >= b->loop_start && pc < b->loop_start + b->loop_len)
                wc = wc % b->loop_len + b->loop_start;
            else if (b->size)
                wc %= b->size;
            *write = wc;
        }
    } else {
        if (play) *play = b->cursor;
        if (write) *write = b->cursor;
    }
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Buf_SetCurrentPosition(void *self, DWORD pos)
{
    BUFFER_ENTRY(b, self);
    if (b->v.flags & DSBCAPS_SUBMIXMASK) VOICE_RETURN(DSERR_INVALIDCALL);
    if (pos % b->v.fmt.align || pos >= b->play_len) VOICE_RETURN(DSERR_INVALIDPARAM);
    struct ds_events ev = { .n = 0 };
    if (b->playing) {
        if (b->looping && pos >= b->loop_start + b->loop_len) b->looping = 0;
        b->pos = bytes_to_frames(&b->v.fmt, pos);
        b->v.adpcm_block = -1;
    } else {
        b->cursor = pos;   /* cached for the next Play */
    }
    buffer_position_delta(b, &ev);
    UNLOCK();
    events_signal(&ev);
    return DS_OK;
}

static HRESULT NTAPI Buf_SetBufferData(void *self, void *pv, DWORD bytes)
{
    struct ds_events ev = { .n = 0 };
    BUFFER_ENTRY(b, self);
    if (b->v.flags & DSBCAPS_SUBMIXMASK) VOICE_RETURN(DSERR_INVALIDCALL);
    if ((pv == NULL) != (bytes == 0)) VOICE_RETURN(DSERR_INVALIDPARAM);
    if (bytes && bytes % b->v.fmt.align) VOICE_RETURN(DSERR_INVALIDPARAM);
    if (pv && b->app_owned && b->data == pv && b->size == bytes) VOICE_RETURN(DS_OK);
    buffer_stop_locked(b, &ev);
    if (b->data && !b->app_owned) { g.mem_allocated -= b->size; pool_free(b->data); }
    b->data = pv; b->size = bytes; b->app_owned = pv != NULL;
    buffer_set_regions_locked(b);
    UNLOCK();
    events_signal(&ev);
    return DS_OK;
}

static HRESULT NTAPI Buf_Lock(void *self, DWORD offset, DWORD bytes, void **pp1, DWORD *pn1, void **pp2, DWORD *pn2,
                              DWORD flags)
{
    if (!pp1 || !pn1 || (!!pp2 != !!pn2) || (flags & ~(DSBLOCK_FROMWRITECURSOR | DSBLOCK_ENTIREBUFFER)))
        return DSERR_INVALIDPARAM;
    BUFFER_ENTRY(b, self);
    if ((b->v.flags & DSBCAPS_SUBMIXMASK) || !b->data) VOICE_RETURN(DSERR_INVALIDCALL);
    if (flags & DSBLOCK_FROMWRITECURSOR) {
        UNLOCK();
        HRESULT hr = Buf_GetCurrentPosition(self, NULL, &offset);
        if (hr != DS_OK) return hr;
        LOCK();
        b = find_buffer(self);
        if (!b) VOICE_RETURN(DSERR_INVALIDPARAM);
    } else if (offset >= b->size || offset % b->v.fmt.align) {
        VOICE_RETURN(DSERR_INVALIDPARAM);
    }
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = b->size;
    else if (!bytes || bytes > b->size || bytes % b->v.fmt.align) VOICE_RETURN(DSERR_INVALIDPARAM);
    *pp1 = b->data + offset;
    *pn1 = bytes < b->size - offset ? bytes : b->size - offset;
    if (pp2) {
        if (*pn1 < bytes) { *pp2 = b->data; *pn2 = bytes - *pn1; }
        else { *pp2 = NULL; *pn2 = 0; }
    }
    b->v.adpcm_block = -1;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Buf_Unlock(void *self, void *p1, DWORD n1, void *p2, DWORD n2)
{
    (void)self; (void)p1; (void)n1; (void)p2; (void)n2;
    LOCK();
    struct ds_buffer *b = find_buffer(self);
    if (b) b->v.adpcm_block = -1;
    UNLOCK();
    return DS_OK;
}


static int notify_cmp(const void *a, const void *b)
{
    DWORD x = ((const struct ds_notify *)a)->offset, y = ((const struct ds_notify *)b)->offset;
    return x < y ? -1 : x > y;
}

static HRESULT NTAPI Buf_SetNotificationPositions(void *self, DWORD count, const DSBPOSITIONNOTIFY *n)
{
    if (count && !n) return DSERR_INVALIDPARAM;
    BUFFER_ENTRY(b, self);
    if (!(b->v.flags & DSBCAPS_CTRLPOSITIONNOTIFY)) VOICE_RETURN(DSERR_CONTROLUNAVAIL);
    for (DWORD i = 0; i < count; i++) {
        if (n[i].dwOffset != DSBPN_OFFSETSTOP &&
            (n[i].dwOffset % b->v.fmt.align || n[i].dwOffset >= b->play_len)) VOICE_RETURN(DSERR_INVALIDPARAM);
        if (!handle_lookup(n[i].hEventNotify)) VOICE_RETURN(DSERR_INVALIDPARAM);
    }
    struct ds_notify *copy = count ? malloc(count * sizeof(*copy)) : NULL;
    for (DWORD i = 0; i < count; i++) copy[i] = (struct ds_notify){ n[i].dwOffset, n[i].hEventNotify };
    if (count) qsort(copy, count, sizeof(*copy), notify_cmp);
    free(b->notify);
    b->notify = copy; b->nnotify = count;
    b->notify_next = 0; b->notify_last = 0xFFFFFFFFu;
    b->notify_active = b->playing && count;
    VOICE_RETURN(DS_OK);
}

/* ---- IDirectSoundStream methods ------------------------------------------------------------- */

#define STREAM_ENTRY(s, self) \
    struct ds_stream *s; LOCK(); s = find_stream(self); \
    if (!s) { UNLOCK(); xlog("DSound: %s on unknown stream %p", __func__ + 7, self); return DSERR_INVALIDPARAM; }

/* Every stream call first delivers that stream's pending completions, as the
   library's passive-level paths do. */
static struct ds_stream *stream_enter(void *self)
{
    LOCK();
    struct ds_stream *s = find_stream(self);
    UNLOCK();
    if (s) stream_drain(s);
    return s;
}

static HRESULT NTAPI Stream_GetInfo(void *self, XMEDIAINFO *info)
{
    if (!info) return DSERR_INVALIDPARAM;
    STREAM_ENTRY(s, self);
    const struct ds_fmt *f = &s->v.fmt;
    DWORD la = (DWORD)f->ch * f->bits / 8 * HW_FRAME;
    if (la < f->align) la = f->align;
    info->dwFlags = XMO_STREAMF_FIXED_SAMPLE_SIZE | XMO_STREAMF_INPUT_ASYNC;
    info->dwInputSize = f->align;
    info->dwOutputSize = 0;
    info->dwMaxLookahead = la * 2;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Stream_GetStatus(void *self, DWORD *st)
{
    if (!st) return DSERR_INVALIDPARAM;
    if (!stream_enter(self)) return DSERR_INVALIDPARAM;
    STREAM_ENTRY(s, self);
    DWORD r = 0;
    if (s->count < s->max_packets) r |= DSSTREAMSTATUS_READY;
    if (s->active) {
        if (s->paused || s->starved) {
            r |= DSSTREAMSTATUS_PAUSED;
            if (s->starved) r |= DSSTREAMSTATUS_STARVED;
        } else {
            r |= DSSTREAMSTATUS_PLAYING;
        }
    }
    *st = r;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Stream_Process(void *self, const XMEDIAPACKET *src, const XMEDIAPACKET *dst)
{
    if (!stream_enter(self)) return DSERR_INVALIDPARAM;
    if (!src || dst || !src->pvBuffer || !src->dwMaxSize) return DSERR_INVALIDPARAM;
    if (src->prtTimestamp && *src->prtTimestamp != 0) return DSERR_INVALIDPARAM;
    STREAM_ENTRY(s, self);
    if (src->dwMaxSize % s->v.fmt.align) VOICE_RETURN(DSERR_INVALIDPARAM);
    if (s->count >= s->max_packets) VOICE_RETURN(DSERR_INVALIDCALL);
    xmo_accept(src);
    struct ds_packet *p = &s->pk[(s->head + s->count) % s->max_packets];
    p->xmp = *src;
    p->frames = bytes_to_frames(&s->v.fmt, src->dwMaxSize);
    s->count++;
    s->discontinuity = 0;
    s->flush_pending = 0;
    s->starved = 0;
    s->active = 1;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Stream_Discontinuity(void *self)
{
    if (!stream_enter(self)) return DSERR_INVALIDPARAM;
    struct ds_done *dl = NULL; DWORD dn = 0;
    STREAM_ENTRY(s, self);
    if (s->count) s->discontinuity = 1;
    else { flush_locked(s, XMEDIAPACKET_STATUS_FLUSHED); dl = stream_take_done(s, &dn); }
    UNLOCK();
    if (dn) deliver(dl, dn);
    free(dl);
    return DS_OK;
}

static HRESULT NTAPI Stream_Flush(void *self)
{
    if (!stream_enter(self)) return DSERR_INVALIDPARAM;
    struct ds_done *dl = NULL; DWORD dn = 0;
    STREAM_ENTRY(s, self);
    flush_locked(s, XMEDIAPACKET_STATUS_FLUSHED);
    dl = stream_take_done(s, &dn);
    UNLOCK();
    if (dn) deliver(dl, dn);
    free(dl);
    return DS_OK;
}

static HRESULT NTAPI Stream_FlushEx(void *self, uint32_t rt_lo, uint32_t rt_hi, DWORD flags)
{
    if (flags & ~(DSSTREAMFLUSHEX_ASYNC | DSSTREAMFLUSHEX_ENVELOPE)) return DSERR_INVALIDPARAM;
    if ((flags & DSSTREAMFLUSHEX_ENVELOPE) && !(flags & DSSTREAMFLUSHEX_ASYNC)) return DSERR_INVALIDPARAM;
    if ((rt_lo || rt_hi) && !(flags & DSSTREAMFLUSHEX_ASYNC)) return DSERR_INVALIDPARAM;
    if (!flags) return Stream_Flush(self);
    if (!stream_enter(self)) return DSERR_INVALIDPARAM;
    STREAM_ENTRY(s, self);
    s->active = 0;
    s->flush_pending = 1;
    VOICE_RETURN(DS_OK);
}

static HRESULT NTAPI Stream_Pause(void *self, DWORD pause)
{
    if (pause > 1) return DSERR_INVALIDPARAM;
    if (!stream_enter(self)) return DSERR_INVALIDPARAM;
    STREAM_ENTRY(s, self);
    s->paused = pause == 1;
    VOICE_RETURN(DS_OK);
}

static void *stream_vtbl[7] = {
    (void *)Obj_AddRef, (void *)Obj_Release, (void *)Stream_GetInfo, (void *)Stream_GetStatus,
    (void *)Stream_Process, (void *)Stream_Discontinuity, (void *)Stream_Flush,
};

static void *wave_vtbl[11] = {
    (void *)Wave_AddRef, (void *)Wave_Release, (void *)Wave_GetInfo, (void *)Wave_GetStatus, (void *)Wave_Process,
    (void *)Wave_Discontinuity, (void *)Wave_Flush, (void *)Wave_Seek, (void *)Wave_GetLength,
    (void *)Wave_GetFormat, (void *)Wave_GetLoopRegion,
};

static void *file_vtbl[9] = {
    (void *)File_AddRef, (void *)File_Release, (void *)File_GetInfo, (void *)File_GetStatus, (void *)File_Process,
    (void *)File_Discontinuity, (void *)File_Flush, (void *)File_Seek, (void *)File_GetLength,
};

/* The guest calls these objects' methods through the vtables: give it
   addresses it can call (cpu.h; nothing to do on x86). */
static void guest_vtbls_once(void)
{
    cpu_guest_table(refhdr_vtbl, 3);
    cpu_guest_table(stream_vtbl, 7);
    cpu_guest_table(wave_vtbl, 11);
    cpu_guest_table(file_vtbl, 9);
}

static void guest_vtbls(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, guest_vtbls_once);
}

/* ---- function table ------------------------------------------------------------------------- */

#define F(n, f) { n, (void *)(f) }

/* A voice-level setter is reached through the buffer thunk, the stream thunk
   and the three C++ classes; they all take the same arguments. */
#define VOICE_SET(name, suffix, impl) \
    F("_IDirectSoundBuffer_" name "@" suffix, impl), \
    F("_IDirectSoundStream_" name "@" suffix, impl)
#define VOICE_CPP(name, sig, impl) \
    F("?" name "@CDirectSoundBuffer@DirectSound@@QAG" sig, impl), \
    F("?" name "@CDirectSoundStream@DirectSound@@QAG" sig, impl), \
    F("?" name "@CDirectSoundVoice@DirectSound@@QAG" sig, impl)

/* XDK 5xxx shared 3D voice data (XACT computes 3D positioning once and
   hands it to its voices).  3D is mixed from each voice's own parameters
   here, so these only answer. */
static HRESULT NTAPI Voice_Use3DVoiceData(void *self, void *data) { (void)self; (void)data; return DS_OK; }
static HRESULT NTAPI Voice_GetVoiceProperties(void *self, void *props)
{
    (void)self;
    if (props) memset(props, 0, 80);   /* DSVOICEPROPS: no mixbins, pitch 0, I3DL2 volumes 0 */
    return DS_OK;
}
/* Buffers played with DSBPLAY_SYNCHPLAYBACK already started. */
static HRESULT NTAPI DS_SynchPlayback(void *self) { (void)self; return DS_OK; }
static HRESULT NTAPI Calc_Calculate3D(DWORD a, void *b) { (void)a; (void)b; return DS_OK; }
static HRESULT NTAPI Calc_GetVoiceData(DWORD a, DWORD b, void *c, void *d, void *e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return DS_OK;
}

/* More XDK 5xxx calls. */
static HRESULT NTAPI Buf_Pause(void *self, DWORD pause)
{
    /* DSBPAUSE_RESUME 0, DSBPAUSE_PAUSE 1, DSBPAUSE_SYNCHPLAYBACK 2 (held
       for SynchPlayback, which starts everything at once here). */
    if (pause > 2) return DSERR_INVALIDPARAM;
    BUFFER_ENTRY(b, self);
    b->paused = pause == 1;
    VOICE_RETURN(DS_OK);
}
static HRESULT NTAPI Buf_PauseEx(void *self, uint32_t rt_lo, uint32_t rt_hi, DWORD pause)
{
    (void)rt_lo; (void)rt_hi;
    return Buf_Pause(self, pause);
}
static HRESULT NTAPI Stream_PauseEx(void *self, uint32_t rt_lo, uint32_t rt_hi, DWORD pause)
{
    (void)rt_lo; (void)rt_hi;
    return Stream_Pause(self, pause == 2 ? 0 : pause);
}
static HRESULT NTAPI Voice_SetRolloffCurve(void *self, const float *points, DWORD count, DWORD apply)
{
    (void)self; (void)points; (void)count; (void)apply;
    return DS_OK;
}
static HRESULT NTAPI DS_GetOutputLevels(void *self, void *levels, DWORD reset)
{
    (void)self; (void)reset;
    if (levels) memset(levels, 0, 16 * sizeof(DWORD));   /* DSOUTPUTLEVELS */
    return DS_OK;
}
/* Buffer data lives in host-visible memory already; mapping is identity. */
static HRESULT NTAPI DS_MapBufferData(void *self, void *data, DWORD bytes, void **mapped)
{
    (void)self; (void)bytes;
    if (mapped) *mapped = data;
    return DS_OK;
}
static HRESULT NTAPI DS_UnmapBufferData(void *self, void *data) { (void)self; (void)data; return DS_OK; }
static HRESULT NTAPI Calc_GetMixBinVolumes(DWORD a, DWORD b, void *c) { (void)a; (void)b; (void)c; return DS_OK; }
static HRESULT NTAPI Calc_GetPanData(DWORD a, DWORD b, DWORD c, void *d) { (void)a; (void)b; (void)c; (void)d; return DS_OK; }
static HRESULT NTAPI XAudioSetEffectData(DWORD index, void *desc, void *raw) { (void)index; (void)desc; (void)raw; return DS_OK; }
static HRESULT NTAPI XFileCreateMediaObjectAsync(HANDLE h, DWORD max_packets, void **ppxmo)
{
    (void)max_packets;
    return XFileCreateMediaObjectEx(h, ppxmo);
}

const struct hle_func dsound_funcs[] = {
    F("_IDirectSoundBuffer_Use3DVoiceData@8", Voice_Use3DVoiceData),
    F("_IDirectSoundBuffer_Set3DVoiceData@8", Voice_Use3DVoiceData),
    F("_IDirectSoundBuffer_GetVoiceProperties@8", Voice_GetVoiceProperties),
    F("_IDirectSoundStream_Use3DVoiceData@8", Voice_Use3DVoiceData),
    F("_IDirectSoundStream_Set3DVoiceData@8", Voice_Use3DVoiceData),
    F("_IDirectSoundStream_GetVoiceProperties@8", Voice_GetVoiceProperties),
    F("_IDirectSound3DCalculator_Calculate3D@8", Calc_Calculate3D),
    F("_IDirectSound_SynchPlayback@4", DS_SynchPlayback),
    F("_CDirectSound_SynchPlayback@4", DS_SynchPlayback),
    F("_IDirectSound3DCalculator_GetVoiceData@20", Calc_GetVoiceData),
    F("_IDirectSound3DCalculator_GetMixBinVolumes@12", Calc_GetMixBinVolumes),
    F("_IDirectSound3DCalculator_GetPanData@16", Calc_GetPanData),
    F("_IDirectSoundBuffer_Pause@8", Buf_Pause),
    F("?Pause@CDirectSoundBuffer@DirectSound@@QAGJK@Z", Buf_Pause),
    F("_IDirectSoundBuffer_PauseEx@16", Buf_PauseEx),
    F("?PauseEx@CDirectSoundBuffer@DirectSound@@QAGJ_JK@Z", Buf_PauseEx),
    F("_IDirectSoundStream_PauseEx@16", Stream_PauseEx),
    F("?PauseEx@CDirectSoundStream@DirectSound@@QAGJ_JK@Z", Stream_PauseEx),
    VOICE_SET("SetMixBinVolumes_8", "8", Voice_SetMixBinVolumes),
    VOICE_CPP("SetMixBinVolumes_8", "JPBU_DSMIXBINS@@@Z", Voice_SetMixBinVolumes),
    VOICE_SET("SetRolloffCurve", "16", Voice_SetRolloffCurve),
    VOICE_CPP("SetRolloffCurve", "JPBMKK@Z", Voice_SetRolloffCurve),
    F("_IDirectSound_GetOutputLevels@12", DS_GetOutputLevels),
    F("?GetOutputLevels@CDirectSound@DirectSound@@QAGJPAU_DSOUTPUTLEVELS@@H@Z", DS_GetOutputLevels),
    F("_IDirectSound_MapBufferData@16", DS_MapBufferData),
    F("?MapBufferData@CDirectSound@DirectSound@@QAGJPAXKPAPAX@Z", DS_MapBufferData),
    F("_IDirectSound_UnmapBufferData@8", DS_UnmapBufferData),
    F("?UnmapBufferData@CDirectSound@DirectSound@@QAGJPAX@Z", DS_UnmapBufferData),
    F("_DirectSoundUseFullHRTF4Channel@0", DirectSoundUseFullHRTF),
    F("_DirectSoundUseLightHRTF4Channel@0", DirectSoundUseLightHRTF),
    F("_XAudioSetEffectData@12", XAudioSetEffectData),
    F("_XFileCreateMediaObjectAsync@12", XFileCreateMediaObjectAsync),
    /* globals */
    F("_DirectSoundCreate@12", DirectSoundCreate),
    F("_DirectSoundCreateBuffer@8", DirectSoundCreateBuffer),
    F("_DirectSoundCreateStream@8", DirectSoundCreateStream),
    F("_DirectSoundDoWork@0", DirectSoundDoWork),
    F("_DirectSoundGetSampleTime@0", DirectSoundGetSampleTime),
    F("_DirectSoundUseFullHRTF@0", DirectSoundUseFullHRTF),
    F("_DirectSoundUseLightHRTF@0", DirectSoundUseLightHRTF),
    F("_DirectSoundUsePan3D@0", DirectSoundUsePan3D),
    F("_DirectSoundOverrideSpeakerConfig@4", DirectSoundOverrideSpeakerConfig),
    F("_DirectSoundDumpMemoryUsage@4", DirectSoundDumpMemoryUsage),
    F("_DirectSoundLoadEncoder@16", DirectSoundLoadEncoder),
    F("_DirectSoundMemAlloc@12", DirectSoundMemAlloc),
    F("_DirectSoundPoolAlloc@12", DirectSoundMemAlloc),
    F("_DirectSoundPhysicalAlloc@16", DirectSoundPhysicalAlloc),
    F("_DirectSoundTrackingMemAlloc@24", DirectSoundTrackingAlloc),
    F("_DirectSoundTrackingPoolAlloc@24", DirectSoundTrackingAlloc),
    F("_DirectSoundTrackingPhysicalAlloc@28", DirectSoundTrackingPhysicalAlloc),
    F("_DirectSoundMemFree@4", DirectSoundMemFree),
    F("_DirectSoundPoolFree@4", DirectSoundMemFree),
    F("_DirectSoundPhysicalFree@4", DirectSoundMemFree),
    F("_DirectSoundTrackingMemFree@4", DirectSoundMemFree),
    F("_DirectSoundTrackingPoolFree@4", DirectSoundMemFree),
    F("_DirectSoundTrackingPhysicalFree@4", DirectSoundMemFree),
    F("_XAudioCalculatePitch@4", XAudioCalculatePitch),
    F("_XAudioCreatePcmFormat@16", XAudioCreatePcmFormat),
    F("_XAudioCreateAdpcmFormat@12", XAudioCreateAdpcmFormat),
    F("_XAudioDownloadEffectsImage@16", XAudioDownloadEffectsImage),
    F("_XWaveFileCreateMediaObject@12", XWaveFileCreateMediaObject),
    F("_XWaveFileCreateMediaObjectEx@12", XWaveFileCreateMediaObjectEx),
    F("_XFileCreateMediaObject@24", XFileCreateMediaObject),
    F("_XFileCreateMediaObjectEx@8", XFileCreateMediaObjectEx),
    F("_Ac97CreateMediaObject@16", Ac97CreateMediaObject),

    /* IDirectSound */
    F("_IDirectSound_QueryInterface@12", Obj_QueryInterface),
    F("_IDirectSound_QueryInterfaceC@12", Obj_QueryInterface),
    F("_IDirectSound_AddRef@4", Obj_AddRef),
    F("_IDirectSound_Release@4", Obj_Release),
    F("?AddRef@CDirectSound@DirectSound@@UAGKXZ", Obj_AddRef),
    F("?Release@CDirectSound@DirectSound@@UAGKXZ", Obj_Release),
    F("?DoWork@CDirectSound@DirectSound@@QAGXXZ", DS_DoWork),
    F("?Force3dRecalc@CDirectSound@DirectSound@@QAGXK@Z", DS_Force3dRecalc),
    F("_IDirectSound_GetCaps@8", DS_GetCaps),
    F("?GetCaps@CDirectSound@DirectSound@@QAGJPAU_DSCAPS@@@Z", DS_GetCaps),
    F("_IDirectSound_CreateSoundBuffer@16", DS_CreateSoundBuffer),
    F("?CreateSoundBuffer@CDirectSound@DirectSound@@QAGJPBU_DSBUFFERDESC@@PAPAUIDirectSoundBuffer@@PAUIUnknown@@@Z",
      DS_CreateSoundBuffer),
    F("_IDirectSound_CreateSoundStream@16", DS_CreateSoundStream),
    F("?CreateSoundStream@CDirectSound@DirectSound@@QAGJPBU_DSSTREAMDESC@@PAPAUIDirectSoundStream@@PAUIUnknown@@@Z",
      DS_CreateSoundStream),
    F("_IDirectSound_GetSpeakerConfig@8", DS_GetSpeakerConfig),
    F("?GetSpeakerConfig@CDirectSound@DirectSound@@QAGJPAK@Z", DS_GetSpeakerConfig),
    F("_IDirectSound_SetCooperativeLevel@12", DS_SetCooperativeLevel),
    F("_IDirectSound_Compact@4", DS_Compact),
    F("_IDirectSound_DownloadEffectsImage@20", DS_DownloadEffectsImage),
    F("?DownloadEffectsImage@CDirectSound@DirectSound@@QAGJPBXKPBU_DSEFFECTIMAGELOC@@PAPAU_DSEFFECTIMAGEDESC@@@Z",
      DS_DownloadEffectsImage),
    F("_IDirectSound_GetEffectData@20", DS_GetEffectData),
    F("?GetEffectData@CDirectSound@DirectSound@@QAGJKKPAXK@Z", DS_GetEffectData),
    F("_IDirectSound_SetEffectData@24", DS_SetEffectData),
    F("?SetEffectData@CDirectSound@DirectSound@@QAGJKKPBXKK@Z", DS_SetEffectData),
    F("_IDirectSound_CommitEffectData@4", DS_CommitEffectData),
    F("?CommitEffectData@CDirectSound@DirectSound@@QAGJXZ", DS_CommitEffectData),
    F("_IDirectSound_EnableHeadphones@8", DS_EnableHeadphones),
    F("?EnableHeadphones@CDirectSound@DirectSound@@QAGJH@Z", DS_EnableHeadphones),
    F("_IDirectSound_SetMixBinHeadroom@12", DS_SetMixBinHeadroom),
    F("?SetMixBinHeadroom@CDirectSound@DirectSound@@QAGJKK@Z", DS_SetMixBinHeadroom),
    F("_IDirectSound_SetAllParameters@12", DS_SetAllParameters),
    F("?SetAllParameters@CDirectSound@DirectSound@@QAGJPBU_DS3DLISTENER@@K@Z", DS_SetAllParameters),
    F("_IDirectSound_SetOrientation@32", DS_SetOrientation),
    F("?SetOrientation@CDirectSound@DirectSound@@QAGJMMMMMMK@Z", DS_SetOrientation),
    F("_IDirectSound_SetPosition@20", DS_SetPosition),
    F("?SetPosition@CDirectSound@DirectSound@@QAGJMMMK@Z", DS_SetPosition),
    F("_IDirectSound_SetVelocity@20", DS_SetVelocity),
    F("?SetVelocity@CDirectSound@DirectSound@@QAGJMMMK@Z", DS_SetVelocity),
    F("_IDirectSound_SetDistanceFactor@12", DS_SetDistanceFactor),
    F("?SetDistanceFactor@CDirectSound@DirectSound@@QAGJMK@Z", DS_SetDistanceFactor),
    F("_IDirectSound_SetDopplerFactor@12", DS_SetDopplerFactor),
    F("?SetDopplerFactor@CDirectSound@DirectSound@@QAGJMK@Z", DS_SetDopplerFactor),
    F("_IDirectSound_SetRolloffFactor@12", DS_SetRolloffFactor),
    F("?SetRolloffFactor@CDirectSound@DirectSound@@QAGJMK@Z", DS_SetRolloffFactor),
    F("_IDirectSound_SetI3DL2Listener@12", DS_SetI3DL2Listener),
    F("?SetI3DL2Listener@CDirectSound@DirectSound@@QAGJPBU_DSI3DL2LISTENER@@K@Z", DS_SetI3DL2Listener),
    F("_IDirectSound_CommitDeferredSettings@4", DS_CommitDeferredSettings),
    F("?CommitDeferredSettings@CDirectSound@DirectSound@@QAGJXZ", DS_CommitDeferredSettings),
    F("_IDirectSound_GetTime@8", DS_GetTime),
    F("?GetTime@CDirectSound@DirectSound@@QAGJPA_J@Z", DS_GetTime),

    /* IDirectSoundBuffer / IDirectSoundStream: refcount and voice settings */
    F("_IDirectSoundBuffer_QueryInterface@12", Obj_QueryInterface),
    F("_IDirectSoundBuffer_QueryInterfaceC@12", Obj_QueryInterface),
    F("_IDirectSoundBuffer_AddRef@4", Obj_AddRef),
    F("_IDirectSoundBuffer_Release@4", Obj_Release),
    F("?AddRef@CDirectSoundBuffer@DirectSound@@UAGKXZ", Obj_AddRef),
    F("?Release@CDirectSoundBuffer@DirectSound@@UAGKXZ", Obj_Release),
    F("_IDirectSoundStream_QueryInterface@12", Obj_QueryInterface),
    F("_IDirectSoundStream_QueryInterfaceC@12", Obj_QueryInterface),
    F("?AddRef@CDirectSoundStream@DirectSound@@UAGKXZ", Obj_AddRef),
    F("?Release@CDirectSoundStream@DirectSound@@UAGKXZ", Obj_Release),
    VOICE_SET("SetFormat", "8", Voice_SetFormat),
    VOICE_CPP("SetFormat", "JPBUtWAVEFORMATEX@@@Z", Voice_SetFormat),
    VOICE_SET("SetFrequency", "8", Voice_SetFrequency),
    VOICE_CPP("SetFrequency", "JK@Z", Voice_SetFrequency),
    VOICE_SET("SetVolume", "8", Voice_SetVolume),
    VOICE_CPP("SetVolume", "JJ@Z", Voice_SetVolume),
    VOICE_SET("SetPitch", "8", Voice_SetPitch),
    VOICE_CPP("SetPitch", "JJ@Z", Voice_SetPitch),
    VOICE_SET("SetLFO", "8", Voice_SetLFO),
    VOICE_CPP("SetLFO", "JPBU_DSLFODESC@@@Z", Voice_SetLFO),
    VOICE_SET("SetEG", "8", Voice_SetEG),
    VOICE_CPP("SetEG", "JPBU_DSENVELOPEDESC@@@Z", Voice_SetEG),
    VOICE_SET("SetFilter", "8", Voice_SetFilter),
    VOICE_CPP("SetFilter", "JPBU_DSFILTERDESC@@@Z", Voice_SetFilter),
    VOICE_SET("SetHeadroom", "8", Voice_SetHeadroom),
    VOICE_CPP("SetHeadroom", "JK@Z", Voice_SetHeadroom),
    VOICE_SET("SetOutputBuffer", "8", Voice_SetOutputBuffer),
    VOICE_CPP("SetOutputBuffer", "JPAUIDirectSoundBuffer@@@Z", Voice_SetOutputBuffer),
    VOICE_SET("SetMixBins", "8", Voice_SetMixBins),
    VOICE_CPP("SetMixBins", "JPBU_DSMIXBINS@@@Z", Voice_SetMixBins),
    VOICE_SET("SetMixBinVolumes", "8", Voice_SetMixBinVolumes),
    VOICE_CPP("SetMixBinVolumes", "JPBU_DSMIXBINS@@@Z", Voice_SetMixBinVolumes),
    VOICE_SET("SetAllParameters", "12", Voice_SetAllParameters),
    VOICE_CPP("SetAllParameters", "JPBU_DS3DBUFFER@@K@Z", Voice_SetAllParameters),
    VOICE_SET("SetConeAngles", "16", Voice_SetConeAngles),
    VOICE_CPP("SetConeAngles", "JKKK@Z", Voice_SetConeAngles),
    VOICE_SET("SetConeOrientation", "20", Voice_SetConeOrientation),
    VOICE_CPP("SetConeOrientation", "JMMMK@Z", Voice_SetConeOrientation),
    VOICE_SET("SetConeOutsideVolume", "12", Voice_SetConeOutsideVolume),
    VOICE_CPP("SetConeOutsideVolume", "JJK@Z", Voice_SetConeOutsideVolume),
    VOICE_SET("SetMaxDistance", "12", Voice_SetMaxDistance),
    VOICE_CPP("SetMaxDistance", "JMK@Z", Voice_SetMaxDistance),
    VOICE_SET("SetMinDistance", "12", Voice_SetMinDistance),
    VOICE_CPP("SetMinDistance", "JMK@Z", Voice_SetMinDistance),
    VOICE_SET("SetMode", "12", Voice_SetMode),
    VOICE_CPP("SetMode", "JKK@Z", Voice_SetMode),
    VOICE_SET("SetPosition", "20", Voice_SetPosition),
    VOICE_CPP("SetPosition", "JMMMK@Z", Voice_SetPosition),
    VOICE_SET("SetVelocity", "20", Voice_SetVelocity),
    VOICE_CPP("SetVelocity", "JMMMK@Z", Voice_SetVelocity),
    VOICE_SET("SetDistanceFactor", "12", Voice_SetDistanceFactor),
    VOICE_CPP("SetDistanceFactor", "JMK@Z", Voice_SetDistanceFactor),
    VOICE_SET("SetDopplerFactor", "12", Voice_SetDopplerFactor),
    VOICE_CPP("SetDopplerFactor", "JMK@Z", Voice_SetDopplerFactor),
    VOICE_SET("SetRolloffFactor", "12", Voice_SetRolloffFactor),
    VOICE_CPP("SetRolloffFactor", "JMK@Z", Voice_SetRolloffFactor),
    VOICE_SET("SetI3DL2Source", "12", Voice_SetI3DL2Source),
    VOICE_CPP("SetI3DL2Source", "JPBU_DSI3DL2BUFFER@@K@Z", Voice_SetI3DL2Source),
    F("?CommitDeferredSettings@CDirectSoundVoice@DirectSound@@QAGJXZ", Voice_CommitDeferredSettings),

    /* IDirectSoundBuffer playback */
    F("_IDirectSoundBuffer_Play@16", Buf_Play),
    F("?Play@CDirectSoundBuffer@DirectSound@@QAGJKKK@Z", Buf_Play),
    F("_IDirectSoundBuffer_PlayEx@16", Buf_PlayEx),
    F("?PlayEx@CDirectSoundBuffer@DirectSound@@QAGJ_JK@Z", Buf_PlayEx),
    F("_IDirectSoundBuffer_Stop@4", Buf_Stop),
    F("?Stop@CDirectSoundBuffer@DirectSound@@QAGJXZ", Buf_Stop),
    F("_IDirectSoundBuffer_StopEx@16", Buf_StopEx),
    F("?StopEx@CDirectSoundBuffer@DirectSound@@QAGJ_JK@Z", Buf_StopEx),
    F("_IDirectSoundBuffer_SetPlayRegion@12", Buf_SetPlayRegion),
    F("?SetPlayRegion@CDirectSoundBuffer@DirectSound@@QAGJKK@Z", Buf_SetPlayRegion),
    F("_IDirectSoundBuffer_SetLoopRegion@12", Buf_SetLoopRegion),
    F("?SetLoopRegion@CDirectSoundBuffer@DirectSound@@QAGJKK@Z", Buf_SetLoopRegion),
    F("_IDirectSoundBuffer_GetStatus@8", Buf_GetStatus),
    F("?GetStatus@CDirectSoundBuffer@DirectSound@@QAGJPAK@Z", Buf_GetStatus),
    F("_IDirectSoundBuffer_GetCurrentPosition@12", Buf_GetCurrentPosition),
    F("?GetCurrentPosition@CDirectSoundBuffer@DirectSound@@QAGJPAK0@Z", Buf_GetCurrentPosition),
    F("_IDirectSoundBuffer_SetCurrentPosition@8", Buf_SetCurrentPosition),
    F("?SetCurrentPosition@CDirectSoundBuffer@DirectSound@@QAGJK@Z", Buf_SetCurrentPosition),
    F("_IDirectSoundBuffer_SetBufferData@12", Buf_SetBufferData),
    F("?SetBufferData@CDirectSoundBuffer@DirectSound@@QAGJPAXK@Z", Buf_SetBufferData),
    F("_IDirectSoundBuffer_Lock@32", Buf_Lock),
    F("?Lock@CDirectSoundBuffer@DirectSound@@QAGJKKPAPAXPAK01K@Z", Buf_Lock),
    F("_IDirectSoundBuffer_Unlock@20", Buf_Unlock),
    F("_IDirectSoundBuffer_Restore@4", DS_Compact),   /* both just return DS_OK */
    F("_IDirectSoundBuffer_SetNotificationPositions@12", Buf_SetNotificationPositions),
    F("?SetNotificationPositions@CDirectSoundBuffer@DirectSound@@QAGJKPBU_DSBPOSITIONNOTIFY@@@Z",
      Buf_SetNotificationPositions),

    /* IDirectSoundStream */
    F("?GetInfo@CDirectSoundStream@DirectSound@@UAGJPAU_XMEDIAINFO@@@Z", Stream_GetInfo),
    F("?GetStatus@CDirectSoundStream@DirectSound@@UAGJPAK@Z", Stream_GetStatus),
    F("?Process@CDirectSoundStream@DirectSound@@UAGJPBU_XMEDIAPACKET@@0@Z", Stream_Process),
    F("?Discontinuity@CDirectSoundStream@DirectSound@@UAGJXZ", Stream_Discontinuity),
    F("?Flush@CDirectSoundStream@DirectSound@@UAGJXZ", Stream_Flush),
    F("_IDirectSoundStream_Pause@8", Stream_Pause),
    F("?Pause@CDirectSoundStream@DirectSound@@QAGJK@Z", Stream_Pause),
    F("_IDirectSoundStream_FlushEx@16", Stream_FlushEx),
    F("?FlushEx@CDirectSoundStream@DirectSound@@QAGJ_JK@Z", Stream_FlushEx),

    { NULL, NULL },
};
