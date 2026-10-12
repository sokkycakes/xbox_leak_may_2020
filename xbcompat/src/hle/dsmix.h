/*
 * The DirectSound mixer's per-sample work: ADPCM decoding, resampling, the
 * voice processor's envelopes and filter, and the sum into the output.
 *
 * dsound.c keeps everything the title sees (objects, cursors, notifications,
 * packet completions) and, each block, describes the voices to mix as an
 * array of struct dsmix_voice.  dsmix_run mixes them and reports back what
 * changed (positions, finished buffers, played packets); dsound.c applies
 * that.  Under box86 the x86 runtime hands the array to the native ARM
 * library (XBR_DS_MIX), which runs this same code natively; everywhere else
 * dsound.c calls dsmix_run itself.
 *
 * Both sides read these structures, so they hold only 4-byte fields and
 * doubles at 8-byte offsets, in structures whose size is a multiple of 8:
 * the i386 and ARM EABI layouts are then the same (checked below).
 */
#ifndef XBC_DSMIX_H
#define XBC_DSMIX_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DSMIX_PITCH_MIN (-32767)
#define DSMIX_PITCH_MAX 8191

/* ---- voice processor state ------------------------------------------------ */

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

struct dsmix_eg { uint32_t d[10]; int32_t seg; uint32_t count; float value, step, coef, target; };

/* A voice's mixer state, kept in the voice between blocks.  dsound.c sets
   the envelopes, filter registers and noteoff from the title's calls. */
#define DSMIX_STATE_FIELDS \
    double step_cur;          /* resampling step at the last control update */ \
    struct dsmix_eg eg[2]; \
    int32_t noteoff;          /* StopEx(ENVELOPE): releasing, off when the amplitude EG ends */ \
    uint32_t ctl;             /* frames since VoiceOn: control-rate phase */ \
    uint32_t fmode, fc0, fc1; /* SetFilter: mode, DLS2 cutoff and resonance registers */ \
    float fc_cur, q_cur, svf_f, svf_q, low[6], band[6]; \
    /* Decoded ADPCM blocks: the last block decoded and the one before it. \
       Interpolation reads frame 63 of one block and frame 0 of the next, \
       and while resampling up it does so for several output frames in a \
       row.  Setting adpcm_block to -1 drops both. */ \
    int16_t adpcm_cache[2 * 64]; \
    int32_t adpcm_block; \
    const uint8_t *adpcm_base; \
    int16_t adpcm_prev_cache[2 * 64]; \
    int32_t adpcm_prev_block;
struct dsmix_state { DSMIX_STATE_FIELDS };

/* ---- one block's work ----------------------------------------------------- */

struct dsmix_packet { const uint8_t *data; uint32_t frames; };

enum { DSMIX_BUFFER, DSMIX_STREAM };

struct dsmix_voice {
    double pos;                       /* in/out: frames from the play start, or into the first packet */
    struct dsmix_state *st;
    const uint8_t *data;              /* buffer: the play region */
    const struct dsmix_packet *pk;    /* stream: the queued packets, oldest first */
    float *sub;                       /* the output (submix) voice's input, or NULL */
    uint32_t kind, ch, bits, align, unit_bytes, adpcm;
    uint32_t total, loop_start, loop_len, looping;   /* buffer, in frames */
    int32_t pitch;                    /* effective pitch, 4096 per octave */
    uint32_t vp;                      /* buffer: run the voice processor */
    uint32_t npk;
    float gain[6][2];                 /* per source channel: left, right */
    float subgain[6];                 /* per source channel: into sub */
    uint32_t playing;                 /* out, buffer: still playing */
    uint32_t consumed;                /* out, stream: packets played to the end */
    uint32_t ran_out;                 /* out, stream: every packet was played */
    uint32_t reserved[2];
};

struct dsmix_job { struct dsmix_voice *voices; uint32_t nvoices; float *out; uint32_t frames; };

_Static_assert(sizeof(struct dsmix_eg) == 64, "dsmix_eg layout");
_Static_assert(offsetof(struct dsmix_state, eg) == 8 && offsetof(struct dsmix_state, adpcm_cache) == 220 &&
               offsetof(struct dsmix_state, adpcm_prev_block) == 740 && sizeof(struct dsmix_state) == 744,
               "dsmix_state layout");
_Static_assert(offsetof(struct dsmix_voice, st) == 8 && offsetof(struct dsmix_voice, kind) == 24 &&
               offsetof(struct dsmix_voice, gain) == 76 && offsetof(struct dsmix_voice, playing) == 148 &&
               sizeof(struct dsmix_voice) == 168, "dsmix_voice layout");
_Static_assert(sizeof(struct dsmix_job) == 16 && sizeof(struct dsmix_packet) == 8, "dsmix_job layout");

/* ---- Xbox ADPCM ------------------------------------------------------------ */

static const int16_t dsmix_adpcm_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t dsmix_adpcm_index_adjust[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline int dsmix_adpcm_sample(int nibble, int *pred, int *idx)
{
    int step = dsmix_adpcm_step[*idx];
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 8) diff = -diff;
    int s = *pred + diff;
    if (s < -32768) s = -32768; else if (s > 32767) s = 32767;
    *pred = s;
    *idx += dsmix_adpcm_index_adjust[nibble];
    if (*idx < 0) *idx = 0; else if (*idx > 88) *idx = 88;
    return s;
}

/* Decode one 64-frame block (36 bytes per channel) to interleaved 16-bit PCM.
   Returns 0 and silence for a corrupt step index, as the library does. */
static inline int dsmix_adpcm_decode_block(const uint8_t *src, int ch, int16_t *out)
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
            out[k] = (int16_t)dsmix_adpcm_sample(n, &pred[0], &idx[0]);
        }
    } else {
        int k = 1;
        for (int grp = 0; grp < 8; grp++) {
            uint32_t l, r;
            memcpy(&l, p + grp * 8, 4);
            memcpy(&r, p + grp * 8 + 4, 4);
            for (int i = 0; i < 8 && k < 64; i++, k++) {
                out[k * 2] = (int16_t)dsmix_adpcm_sample(l & 0xF, &pred[0], &idx[0]);
                out[k * 2 + 1] = (int16_t)dsmix_adpcm_sample(r & 0xF, &pred[1], &idx[1]);
                l >>= 4; r >>= 4;
            }
        }
    }
    return 1;
}

/* ---- envelopes and filter ---------------------------------------------------- */

static void dsmix_eg_enter(struct dsmix_eg *e, int seg)
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

static inline float dsmix_eg_tick(struct dsmix_eg *e)
{
    float v = e->value;
    switch (e->seg) {
    case EG_DELAY: if (--e->count == 0) dsmix_eg_enter(e, EG_ATTACK); break;
    case EG_ATTACK: e->value += e->step; if (--e->count == 0) dsmix_eg_enter(e, EG_HOLD); break;
    case EG_HOLD: if (--e->count == 0) dsmix_eg_enter(e, EG_DECAY); break;
    case EG_DECAY:
        e->value = e->target + (e->value - e->target) * e->coef;
        if (--e->count == 0) dsmix_eg_enter(e, EG_SUSTAIN);
        break;
    case EG_RELEASE: e->value *= e->coef; if (--e->count == 0) dsmix_eg_enter(e, EG_OFF); break;
    }
    return v;
}

static inline float dsmix_s16field(uint32_t v) { return (float)(int16_t)(uint16_t)(v & 0xFFFF); }
static inline float dsmix_s8field(uint32_t v) { return (float)(int8_t)(uint8_t)(v & 0xFF); }

/* Per 32-frame update: pitch and cutoff with the multi-function EG. */
static void dsmix_frame_params(struct dsmix_state *v, int32_t pitch)
{
    const struct dsmix_eg *m = &v->eg[DSEG_IDX_MULTI];
    float env = m->value;
    float p = pitch + dsmix_s8field(m->d[EGD_PITCH]) * 32.0f * env;
    if (p < DSMIX_PITCH_MIN) p = DSMIX_PITCH_MIN;
    if (p > DSMIX_PITCH_MAX) p = DSMIX_PITCH_MAX;
    v->step_cur = exp2(p / 4096.0);
    if (v->fmode == 1) {   /* DSFILTER_MODE_DLS2; other modes are passed through */
        v->fc_cur += (dsmix_s16field(v->fc0) - v->fc_cur) * 0.25f;
        v->q_cur += ((v->fc1 & 0xFFFF) / 32768.0f - v->q_cur) * 0.25f;
        float fc = v->fc_cur + dsmix_s8field(m->d[EGD_FC]) * 256.0f * env;
        if (fc > 0.0f) fc = 0.0f;
        if (fc < -32768.0f) fc = -32768.0f;
        v->svf_f = exp2f(fc / 4096.0f);
        v->svf_q = v->q_cur < 0.02f ? 0.02f : v->q_cur > 2.0f ? 2.0f : v->q_cur;
    }
}

/* ---- sample fetch ---------------------------------------------------------- */

struct dsmix_src {
    const uint8_t *data;
    const struct dsmix_voice *j;
    struct dsmix_state *st;
};

static inline void dsmix_fetch(const struct dsmix_src *s, uint32_t idx, float *out)
{
    const struct dsmix_voice *j = s->j;
    int ch = (int)j->ch;
    if (j->adpcm) {
        int32_t block = (int32_t)(idx / 64);
        struct dsmix_state *v = s->st;
        const int16_t *cache = v->adpcm_cache;
        if (v->adpcm_block < 0 || v->adpcm_base != s->data) {
            v->adpcm_prev_block = -1;
            v->adpcm_block = -1;
        }
        if (v->adpcm_block != block) {
            if (v->adpcm_prev_block == block) {
                cache = v->adpcm_prev_cache;
            } else {
                if (v->adpcm_block >= 0) {
                    memcpy(v->adpcm_prev_cache, v->adpcm_cache, sizeof v->adpcm_cache);
                    v->adpcm_prev_block = v->adpcm_block;
                }
                dsmix_adpcm_decode_block(s->data + (uint32_t)block * j->unit_bytes, ch, v->adpcm_cache);
                v->adpcm_block = block;
                v->adpcm_base = s->data;
            }
        }
        const int16_t *c = cache + (idx % 64) * ch;
        for (int i = 0; i < ch; i++) out[i] = c[i] * (1.0f / 32768.0f);
    } else if (j->bits == 16) {
        const uint8_t *p = s->data + idx * j->align;
        for (int i = 0; i < ch; i++) {
            int16_t x; memcpy(&x, p + 2 * i, 2);
            out[i] = x * (1.0f / 32768.0f);
        }
    } else {
        const uint8_t *p = s->data + idx * j->align;
        for (int i = 0; i < ch; i++) out[i] = (p[i] - 128) * (1.0f / 128.0f);
    }
}

/* ---- voices ---------------------------------------------------------------- */

static void dsmix_buffer(struct dsmix_voice *j, float *out, int n)
{
    struct dsmix_state *v = j->st;
    uint32_t total = j->total, ls = j->loop_start, ll = j->loop_len;
    int looping = j->looping && ll > 0 && ls + ll <= total;
    uint32_t le = ls + ll;
    int32_t pitch = j->pitch;
    double step = exp2((double)pitch / 4096.0);
    int vp = (int)j->vp;
    struct dsmix_src src = { j->data, j, v };
    int ch = (int)j->ch;
    float *sub = j->sub;
    double pos = j->pos;
    int playing = 1;
    float a[6], c[6];
    for (int i = 0; i < n; i++) {
        if (looping && pos >= le) pos = ls + fmod(pos - ls, (double)ll);
        if (pos >= total) { playing = 0; pos = 0; break; }
        uint32_t idx = (uint32_t)pos;
        float frac = (float)(pos - idx);
        uint32_t nxt = idx + 1;
        if (looping && nxt >= le) nxt = ls;
        if (nxt >= total) nxt = idx;
        dsmix_fetch(&src, idx, a);
        dsmix_fetch(&src, nxt, c);
        float l = 0, r = 0, m = 0, amp = 1.0f;
        if (vp) {
            if ((v->ctl++ & 31) == 0) dsmix_frame_params(v, pitch);
            step = v->step_cur;
            amp = dsmix_eg_tick(&v->eg[DSEG_IDX_AMP]);
            dsmix_eg_tick(&v->eg[DSEG_IDX_MULTI]);
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
            l += s * j->gain[k][0];
            r += s * j->gain[k][1];
            m += s * j->subgain[k];
        }
        out[2 * i] += l;
        out[2 * i + 1] += r;
        if (sub) sub[i] += m;
        if (v->noteoff && v->eg[DSEG_IDX_AMP].seg == EG_OFF) {   /* end of release: voice off */
            playing = 0;
            break;
        }
        pos += step;
    }
    if (playing) {
        if (looping && pos >= le) pos = ls + fmod(pos - ls, (double)ll);
        j->pos = pos;
    } else {
        j->pos = 0;
    }
    j->playing = (uint32_t)playing;
}

static void dsmix_stream(struct dsmix_voice *j, float *out, int n)
{
    struct dsmix_state *v = j->st;
    double step = exp2((double)j->pitch / 4096.0);
    int ch = (int)j->ch;
    float *sub = j->sub;
    double pos = j->pos;
    uint32_t head = 0;
    float a[6], c[6];
    j->consumed = 0;
    j->ran_out = 0;
    for (int i = 0; i < n; i++) {
        const struct dsmix_packet *p = &j->pk[head];
        while (pos >= p->frames) {
            pos -= p->frames;
            head++;
            j->consumed = head;
            v->adpcm_block = -1;
            if (head == j->npk) { j->ran_out = 1; j->pos = pos; return; }
            p = &j->pk[head];
        }
        struct dsmix_src src = { p->data, j, v };
        uint32_t idx = (uint32_t)pos;
        float frac = (float)(pos - idx);
        uint32_t nxt = idx + 1 < p->frames ? idx + 1 : idx;
        dsmix_fetch(&src, idx, a);
        dsmix_fetch(&src, nxt, c);
        float l = 0, r = 0, m = 0;
        for (int k = 0; k < ch; k++) {
            float smp = a[k] + (c[k] - a[k]) * frac;
            l += smp * j->gain[k][0];
            r += smp * j->gain[k][1];
            m += smp * j->subgain[k];
        }
        out[2 * i] += l;
        out[2 * i + 1] += r;
        if (sub) sub[i] += m;
        pos += step;
    }
    j->pos = pos;
}

/* Mix the block's voices into out (stereo; summed into, not cleared). */
static void dsmix_run(const struct dsmix_job *job)
{
    for (uint32_t i = 0; i < job->nvoices; i++) {
        struct dsmix_voice *j = &job->voices[i];
        if (j->kind == DSMIX_STREAM) dsmix_stream(j, job->out, (int)job->frames);
        else dsmix_buffer(j, job->out, (int)job->frames);
    }
}

#endif
