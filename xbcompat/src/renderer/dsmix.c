/* The DirectSound mixer's per-sample work, natively (XBR_DS_MIX).

   With many voices the block is split across helper threads: each helper
   mixes its share of the voices into a buffer of its own, which the caller
   adds into the output when they finish.  Voices that also feed a submix
   voice stay on the caller, so a submix input has a single writer.  Every
   voice keeps its own state, so positions, envelopes and the rest come out
   exactly as with one thread; only the order the voices are summed in
   differs.  XBCOMPAT_MIX_THREADS sets the threads in all (1 = no helpers).

   Calls come from one thread at a time (dsound.c holds its lock). */
#define _GNU_SOURCE
#include "../hle/dsmix.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <unistd.h>

void renderer_ds_mix(struct dsmix_job *job);

#define MIX_MAX_THREADS 4
/* Below this much work the wake-ups cost more than they save. */
#define MIX_SPLIT_WEIGHT 48

struct mix_worker {
    pthread_t thread;
    sem_t go, done;
    struct dsmix_voice *voices;
    uint32_t *list, n, cap;
    float *out;
    uint32_t out_cap, frames;
};

static struct mix_worker workers[MIX_MAX_THREADS];
static int nthreads;   /* the caller included; 0 until set up */

static void mix_list(struct dsmix_voice *voices, const uint32_t *list, uint32_t n, float *out, uint32_t frames)
{
    for (uint32_t i = 0; i < n; i++) {
        struct dsmix_voice *j = &voices[list[i]];
        if (j->kind == DSMIX_STREAM) dsmix_stream(j, out, (int)frames);
        else dsmix_buffer(j, out, (int)frames);
    }
}

static void *mix_thread(void *arg)
{
    struct mix_worker *w = arg;
    pthread_setname_np(pthread_self(), "ds-mix");
    for (;;) {
        while (sem_wait(&w->go) != 0) {}
        memset(w->out, 0, (size_t)w->frames * 2 * sizeof(float));
        mix_list(w->voices, w->list, w->n, w->out, w->frames);
        sem_post(&w->done);
    }
    return NULL;
}

static void mix_setup(void)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int want = cpus >= 4 ? 3 : cpus >= 2 ? 2 : 1;
    const char *e = getenv("XBCOMPAT_MIX_THREADS");
    if (e && *e) want = atoi(e);
    if (want < 1) want = 1;
    if (want > MIX_MAX_THREADS) want = MIX_MAX_THREADS;
    nthreads = 1;
    for (int i = 1; i < want; i++) {
        struct mix_worker *w = &workers[i];
        sem_init(&w->go, 0, 0);
        sem_init(&w->done, 0, 0);
        if (pthread_create(&w->thread, NULL, mix_thread, w) != 0) break;
        nthreads = i + 1;
    }
}

/* Rough cost per frame: decoding ADPCM costs about twice reading PCM, per
   channel, and the voice processor's filter a little more. */
static uint32_t voice_weight(const struct dsmix_voice *j)
{
    uint32_t w = (j->adpcm ? 4 : 2) * (j->ch ? j->ch : 1);
    if (j->kind == DSMIX_BUFFER && j->vp && j->st->fmode == 1) w++;
    return w;
}

static void list_add(struct mix_worker *w, uint32_t v)
{
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        w->list = realloc(w->list, w->cap * sizeof(*w->list));
    }
    w->list[w->n++] = v;
}

void renderer_ds_mix(struct dsmix_job *job)
{
    if (!nthreads) mix_setup();
    uint32_t total = 0;
    for (uint32_t i = 0; i < job->nvoices; i++) total += voice_weight(&job->voices[i]);
    if (nthreads < 2 || total < MIX_SPLIT_WEIGHT) { dsmix_run(job); return; }

    /* Submix feeders on the caller; the rest to whichever thread has the
       least work so far, in voice order (deterministic). */
    uint32_t load[MIX_MAX_THREADS] = { 0 };
    for (int t = 0; t < nthreads; t++) workers[t].n = 0;
    for (uint32_t i = 0; i < job->nvoices; i++)
        if (job->voices[i].sub) { list_add(&workers[0], i); load[0] += voice_weight(&job->voices[i]); }
    for (uint32_t i = 0; i < job->nvoices; i++) {
        if (job->voices[i].sub) continue;
        int best = 0;
        for (int t = 1; t < nthreads; t++) if (load[t] < load[best]) best = t;
        list_add(&workers[best], i);
        load[best] += voice_weight(&job->voices[i]);
    }

    int started[MIX_MAX_THREADS] = { 0 };
    for (int t = 1; t < nthreads; t++) {
        struct mix_worker *w = &workers[t];
        if (!w->n) continue;
        if (w->out_cap < job->frames) {
            free(w->out);
            w->out = malloc((size_t)job->frames * 2 * sizeof(float));
            w->out_cap = job->frames;
        }
        w->voices = job->voices;
        w->frames = job->frames;
        started[t] = 1;
        sem_post(&w->go);
    }
    mix_list(job->voices, workers[0].list, workers[0].n, job->out, job->frames);
    for (int t = 1; t < nthreads; t++) {
        if (!started[t]) continue;
        struct mix_worker *w = &workers[t];
        while (sem_wait(&w->done) != 0) {}
        for (uint32_t i = 0; i < 2 * job->frames; i++) job->out[i] += w->out[i];
    }
}
