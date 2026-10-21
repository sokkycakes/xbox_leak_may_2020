//
//  platform_alsa.cpp
//
//  Audio straight to ALSA, for systems without SDL2 (a bare console). A
//  thread pulls 48 kHz stereo from the callback and writes it to the PCM.
//
//  Started from init, bootani can be running before the kernel has finished
//  registering the sound card (HD Audio probes its codecs in the background,
//  and HDMI audio waits for the GPU driver). So when no device opens, the
//  thread keeps consuming the sound in real time and retries for a while;
//  once the card appears, the sound picks up in step with the picture.
//
#include "backends.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>

static snd_pcm_t*            g_pcm;
static pthread_t             g_thread;
static pthread_mutex_t       g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool         g_running;
static PlatformAudioCallback g_cb;
static void*                 g_user;
static snd_pcm_uframes_t     g_period = 1024;
static int                   g_rate;
static const char*           g_device;

static const int kRetrySeconds = 15;

static bool TryOpen(const char* name, int rate);
static void UnmuteCard();

static void QuietAlsa(const char*, int, const char*, int, const char*, ...) {}

static bool OpenAny(bool report)
{
    // An explicit device, else the ALSA default, else the first HDMI output
    // of an Intel HD Audio controller (the only output on many mini PCs).
    const char* candidates[] = { g_device, "default", "hdmi:CARD=PCH,DEV=0", "plughw:0,3", "plughw:0,0" };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (!candidates[i] || !*candidates[i]) continue;
        if (TryOpen(candidates[i], g_rate)) return true;
        if (report && i == 0)
            fprintf(stderr, "bootani: ALSA device %s unavailable, trying others\n", g_device);
    }
    return false;
}

static double Now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void* AudioThread(void*)
{
    std::vector<int16_t> buf(g_period * 2);
    // No device yet: play into nothing at the real rate while retrying.
    const double start = Now();
    double next_retry = start, played = 0.0;
    while (g_running && !g_pcm) {
        double now = Now();
        if (now >= next_retry) {
            // ALSA reads each card's configuration once, when it first loads
            // its config; drop it so a card that just appeared is seen.
            snd_config_update_free_global();
            if (OpenAny(false)) {
                fprintf(stderr, "bootani: sound device appeared after %.1f s\n", now - start);
                snd_lib_error_set_handler(NULL);
                buf.resize(g_period * 2);
                break;
            }
            if (now - start > kRetrySeconds) {
                fprintf(stderr, "bootani: no sound device appeared, continuing without sound\n");
                g_running = false;
                return NULL;
            }
            next_retry = now + 0.25;
        }
        pthread_mutex_lock(&g_lock);
        g_cb(&buf[0], (int)g_period, g_user);
        pthread_mutex_unlock(&g_lock);
        played += (double)g_period / g_rate;
        double ahead = start + played - Now();
        if (ahead > 0) {
            struct timespec ts = { (time_t)ahead, (long)((ahead - (time_t)ahead) * 1e9) };
            nanosleep(&ts, NULL);
        }
    }
    while (g_running) {
        pthread_mutex_lock(&g_lock);
        g_cb(&buf[0], (int)g_period, g_user);
        pthread_mutex_unlock(&g_lock);
        const int16_t* p = &buf[0];
        snd_pcm_uframes_t left = g_period;
        while (left > 0 && g_running) {
            snd_pcm_sframes_t n = snd_pcm_writei(g_pcm, p, left);
            if (n < 0) {
                if (snd_pcm_recover(g_pcm, (int)n, 1) < 0) { g_running = false; break; }
                continue;
            }
            p += n * 2;
            left -= n;
        }
    }
    return NULL;
}

static bool TryOpen(const char* name, int rate)
{
    if (snd_pcm_open(&g_pcm, name, SND_PCM_STREAM_PLAYBACK, 0) < 0) { g_pcm = NULL; return false; }
    // Resampling allowed, about 40 ms of latency.
    if (snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, 40000) < 0) {
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return false;
    }
    snd_pcm_uframes_t buffer = 0, period = 0;
    if (snd_pcm_get_params(g_pcm, &buffer, &period) == 0 && period > 0) g_period = period;
    UnmuteCard();
    return true;
}

// The kernel brings sound cards up muted, and at boot nothing has restored
// saved mixer settings yet. Turn up the playback controls of the card we
// opened that are still in that state (switch off and volume at minimum),
// leaving anything someone has already set alone.
static void UnmuteCard()
{
    snd_pcm_info_t* info;
    snd_pcm_info_alloca(&info);
    if (snd_pcm_info(g_pcm, info) < 0) return;
    int card = snd_pcm_info_get_card(info);
    if (card < 0) return;   // not a hardware card (null, file, ...)
    char name[32];
    snprintf(name, sizeof(name), "hw:%d", card);

    snd_mixer_t* mixer;
    if (snd_mixer_open(&mixer, 0) < 0) return;
    if (snd_mixer_attach(mixer, name) < 0 || snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
        snd_mixer_load(mixer) < 0) {
        snd_mixer_close(mixer);
        return;
    }
    for (snd_mixer_elem_t* e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e)) {
        if (!snd_mixer_selem_is_active(e)) continue;
        bool has_switch = snd_mixer_selem_has_playback_switch(e);
        bool has_volume = snd_mixer_selem_has_playback_volume(e);
        if (!has_switch && !has_volume) continue;
        int on = 1;
        if (has_switch) snd_mixer_selem_get_playback_switch(e, SND_MIXER_SCHN_FRONT_LEFT, &on);
        long vmin = 0, vmax = 0, v = 0;
        if (has_volume) {
            snd_mixer_selem_get_playback_volume_range(e, &vmin, &vmax);
            snd_mixer_selem_get_playback_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &v);
        }
        bool untouched = has_switch ? (!on && (!has_volume || v <= vmin)) : (v <= vmin);
        if (!untouched) continue;
        if (has_volume && snd_mixer_selem_set_playback_dB_all(e, 0, 1) < 0)
            snd_mixer_selem_set_playback_volume_all(e, vmin + (vmax - vmin) * 4 / 5);
        if (has_switch) snd_mixer_selem_set_playback_switch_all(e, 1);
    }
    snd_mixer_close(mixer);
}

bool Alsa_AudioOpen(const char* device, int rate, PlatformAudioCallback cb, void* user)
{
    g_device = device;
    g_rate = rate;
    if (!OpenAny(true)) {
        snd_lib_error_set_handler(QuietAlsa);   // the retries would repeat ALSA's complaints
        fprintf(stderr, "bootani: no ALSA playback device yet, will keep trying for %d s\n", kRetrySeconds);
    }
    g_cb = cb;
    g_user = user;
    g_running = true;
    if (pthread_create(&g_thread, NULL, AudioThread, NULL) != 0) {
        g_running = false;
        if (g_pcm) snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return false;
    }
    return true;
}

void Alsa_AudioLock()   { pthread_mutex_lock(&g_lock); }
void Alsa_AudioUnlock() { pthread_mutex_unlock(&g_lock); }

void Alsa_AudioClose()
{
    g_running = false;
    pthread_join(g_thread, NULL);
    if (g_pcm) {
        snd_pcm_drop(g_pcm);
        snd_pcm_close(g_pcm);
    }
    g_pcm = NULL;
}
