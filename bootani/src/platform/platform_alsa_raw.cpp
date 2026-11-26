//
//  platform_alsa_raw.cpp
//
//  The ALSA backend without libasound: 48 kHz stereo S16 straight to a
//  kernel PCM device (/dev/snd/pcmC<card>D<dev>p) through its ioctls, the way
//  tinyalsa does. For tiny images (the Sion's LinuxBoot firmware) where
//  libasound and its configuration files don't fit. Same behaviour as
//  platform_alsa.cpp: the device is retried while the sound card is still
//  probing, and the sound keeps time with the picture meanwhile.
//
//  --audio-device takes hw:C,D (or C,D). Without it: the first HDMI output
//  of the first card (device 3, Intel HD Audio's first HDMI port), then any
//  playback device.
//
#include "backends.h"
#include <sound/asound.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <vector>

static int                   g_fd = -1;
static pthread_t             g_thread;
static pthread_mutex_t       g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool         g_running;
static PlatformAudioCallback g_cb;
static void*                 g_user;
static unsigned              g_period = 1024;
static int                   g_rate;
static const char*           g_device;

static const int kRetrySeconds = 15;

static snd_mask* Mask(snd_pcm_hw_params* p, int n) { return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]; }
static snd_interval* Interval(snd_pcm_hw_params* p, int n) { return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]; }

static void SetMask(snd_pcm_hw_params* p, int n, unsigned bit)
{
    snd_mask* m = Mask(p, n);
    memset(m->bits, 0, sizeof(m->bits));
    m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void SetInt(snd_pcm_hw_params* p, int n, unsigned v)
{
    snd_interval* i = Interval(p, n);
    i->min = i->max = v;
    i->integer = 1;
}

static void SetMin(snd_pcm_hw_params* p, int n, unsigned v)
{
    Interval(p, n)->min = v;
}

static void InitParams(snd_pcm_hw_params* p)
{
    memset(p, 0, sizeof(*p));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(Mask(p, n)->bits, 0xff, sizeof(Mask(p, n)->bits));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        Interval(p, n)->min = 0;
        Interval(p, n)->max = UINT_MAX;
    }
    p->rmask = ~0u;
    p->info = ~0u;
}

static void UnmuteCard(int card);

static bool OpenPcm(int card, int dev)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dp", card, dev);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;

    snd_pcm_hw_params hw;
    InitParams(&hw);
    SetMask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    SetMask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    SetMask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    SetInt(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    SetInt(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32);
    SetInt(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    SetInt(&hw, SNDRV_PCM_HW_PARAM_RATE, (unsigned)g_rate);
    SetMin(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 256);
    Interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max = 1024;
    SetMin(&hw, SNDRV_PCM_HW_PARAM_PERIODS, 2);
    Interval(&hw, SNDRV_PCM_HW_PARAM_PERIODS)->max = 4;
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        fprintf(stderr, "bootani: %s: 48 kHz stereo 16-bit refused: %s\n", path, strerror(errno));
        close(fd);
        return false;
    }
    unsigned period = Interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max;
    unsigned buffer = Interval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->max;

    snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = period;
    sw.start_threshold = period;
    sw.stop_threshold = buffer;
    sw.boundary = buffer;
    while (sw.boundary * 2 <= (unsigned long)LONG_MAX - buffer) sw.boundary *= 2;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0 || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        close(fd);
        return false;
    }
    g_period = period;
    g_fd = fd;
    UnmuteCard(card);
    return true;
}

static bool OpenAny(bool report)
{
    int card = -1, dev = -1;
    if (g_device && *g_device) {
        const char* s = strncmp(g_device, "hw:", 3) == 0 ? g_device + 3 : g_device;
        if (sscanf(s, "%d,%d", &card, &dev) == 2) {
            if (OpenPcm(card, dev)) return true;
            if (report) fprintf(stderr, "bootani: sound device %s unavailable, trying others\n", g_device);
        }
    }
    if (OpenPcm(0, 3)) return true;
    DIR* d = opendir("/dev/snd");
    if (!d) return false;
    bool ok = false;
    while (struct dirent* e = readdir(d)) {
        int c, n;
        char p;
        if (sscanf(e->d_name, "pcmC%dD%d%c", &c, &n, &p) == 3 && p == 'p' && OpenPcm(c, n)) { ok = true; break; }
    }
    closedir(d);
    return ok;
}

// The kernel brings cards up muted. Switch on playback switches that are all
// off (HDMI's "IEC958 Playback Switch", analog "Master Playback Switch"...).
static void UnmuteCard(int card)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    snd_ctl_elem_list list;
    memset(&list, 0, sizeof(list));
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0 && list.count > 0) {
        std::vector<snd_ctl_elem_id> ids(list.count);
        list.space = list.count;
        list.pids = &ids[0];
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
            for (unsigned i = 0; i < list.used; i++) {
                if (!strstr((const char*)ids[i].name, "Playback Switch")) continue;
                snd_ctl_elem_info info;
                memset(&info, 0, sizeof(info));
                info.id = ids[i];
                if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 || info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN) continue;
                snd_ctl_elem_value v;
                memset(&v, 0, sizeof(v));
                v.id = info.id;
                if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0) continue;
                bool any = false;
                for (unsigned c = 0; c < info.count && c < 128; c++) any |= v.value.integer.value[c] != 0;
                if (any) continue;
                for (unsigned c = 0; c < info.count && c < 128; c++) v.value.integer.value[c] = 1;
                ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
            }
        }
    }
    close(fd);
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
    const double start = Now();
    double next_retry = start, played = 0.0;
    while (g_running && g_fd < 0) {
        double now = Now();
        if (now >= next_retry) {
            if (OpenAny(false)) {
                fprintf(stderr, "bootani: sound device appeared after %.1f s\n", now - start);
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
        int16_t* p = &buf[0];
        unsigned left = g_period;
        while (left > 0 && g_running) {
            snd_xferi x;
            memset(&x, 0, sizeof(x));
            x.buf = p;
            x.frames = left;
            if (ioctl(g_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
                if (errno == EPIPE || errno == ESTRPIPE) {   // underrun: start again
                    if (ioctl(g_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { g_running = false; break; }
                    continue;
                }
                if (errno == EINTR) continue;
                g_running = false;
                break;
            }
            p += x.result * 2;
            left -= (unsigned)x.result;
        }
    }
    return NULL;
}

bool Alsa_AudioOpen(const char* device, int rate, PlatformAudioCallback cb, void* user)
{
    g_device = device;
    g_rate = rate;
    if (!OpenAny(true))
        fprintf(stderr, "bootani: no sound device yet, will keep trying for %d s\n", kRetrySeconds);
    g_cb = cb;
    g_user = user;
    g_running = true;
    if (pthread_create(&g_thread, NULL, AudioThread, NULL) != 0) {
        g_running = false;
        if (g_fd >= 0) close(g_fd);
        g_fd = -1;
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
    if (g_fd >= 0) {
        ioctl(g_fd, SNDRV_PCM_IOCTL_DROP);
        close(g_fd);
    }
    g_fd = -1;
}
