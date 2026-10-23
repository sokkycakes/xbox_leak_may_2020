// The game's sound on the Sion (see soundout.h). The PCM setup is the kernel's own
// interface (sound/asound.h), as tinyalsa does it, so the static mirrorrun needs no
// ALSA library or configuration: the dashboard's device name ($AUDIODEV) only picks
// the card and device.

#include "soundout.h"

#include <sound/asound.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const uint32_t RingFrames = 16384;
// frames in the buffer the rate control aims for (53 ms; MIRROR_SOUNDFILL): a slow frame
// (a course loading) mustn't empty it
static const double TargetFill = getenv("MIRROR_SOUNDFILL") ? atof(getenv("MIRROR_SOUNDFILL")) : 2560;

// ---- the PCM device

static snd_mask* Mask(snd_pcm_hw_params* p, int n) { return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]; }
static snd_interval* Interval(snd_pcm_hw_params* p, int n) { return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]; }
static void SetMask(snd_pcm_hw_params* p, int n, unsigned bit)
{
    snd_mask* m = Mask(p, n);
    memset(m->bits, 0, sizeof m->bits);
    m->bits[bit >> 5] |= 1u << (bit & 31);
}
static void SetInt(snd_pcm_hw_params* p, int n, unsigned v)
{
    snd_interval* i = Interval(p, n);
    i->min = i->max = v;
    i->integer = 1;
}

// $AUDIODEV as the dashboard has it (hdmi:CARD=PCH,DEV=0, hw:0,3, plughw:CARD=X,DEV=n)
// to /dev/snd/pcmC<card>D<device>p; with nothing set, the first device that plays
static std::string PcmPath()
{
    if (const char* p = getenv("MIRROR_PCM")) return p;
    std::string dev = getenv("AUDIODEV") ? getenv("AUDIODEV") : "";
    int card = -1, num = -1, hdmi = -1;
    auto field = [&](const char* key) -> std::string {
        size_t at = dev.find(key);
        if (at == std::string::npos) return "";
        at += strlen(key);
        return dev.substr(at, dev.find(',', at) - at);
    };
    std::string cardName = field("CARD=");
    std::string devField = field("DEV=");
    if (dev.rfind("hdmi:", 0) == 0) hdmi = devField.empty() ? 0 : atoi(devField.c_str());
    else if (!devField.empty()) num = atoi(devField.c_str());
    else if (sscanf(dev.c_str(), "%*[^:]:%d,%d", &card, &num) < 2) num = -1;
    if (!cardName.empty())
    {
        // " 0 [PCH            ]: HDA-Intel - HDA Intel PCH"
        if (FILE* f = fopen("/proc/asound/cards", "r"))
        {
            char line[256], name[64];
            int n;
            while (fgets(line, sizeof line, f))
                if (sscanf(line, " %d [%63[^] ]", &n, name) == 2 && cardName == name) { card = n; break; }
            fclose(f);
        }
    }
    // "00-03: HDMI 0 : HDMI 0 : playback 1"
    if (FILE* f = fopen("/proc/asound/pcm", "r"))
    {
        char line[256];
        int c, d;
        while (fgets(line, sizeof line, f))
        {
            if (sscanf(line, "%d-%d:", &c, &d) != 2 || !strstr(line, "playback")) continue;
            if (card >= 0 && c != card) continue;
            if (hdmi >= 0)
            {
                char want[32];
                snprintf(want, sizeof want, ": HDMI %d ", hdmi);
                if (!strstr(line, want)) continue;
            }
            else if (num >= 0 && d != num) continue;
            card = c;
            num = d;
            break;
        }
        fclose(f);
    }
    if (card < 0 || num < 0) return "";
    char path[64];
    snprintf(path, sizeof path, "/dev/snd/pcmC%dD%dp", card, num);
    return path;
}

bool SoundOut::OpenPcm(const std::string& path)
{
    pcm = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (pcm < 0) { fprintf(stderr, "sound: %s: %s\n", path.c_str(), strerror(errno)); return false; }
    snd_pcm_hw_params hp;
    memset(&hp, 0, sizeof hp);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(Mask(&hp, n)->bits, 0xFF, sizeof Mask(&hp, n)->bits);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++)
    {
        Interval(&hp, n)->min = 0;
        Interval(&hp, n)->max = ~0u;
    }
    hp.rmask = ~0u;
    hp.cmask = 0;
    hp.info = ~0u;
    SetMask(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    SetMask(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    SetMask(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    Interval(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min = period;
    SetInt(&hp, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    SetInt(&hp, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32);
    SetInt(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    SetInt(&hp, SNDRV_PCM_HW_PARAM_PERIODS, periods);
    SetInt(&hp, SNDRV_PCM_HW_PARAM_RATE, Rate);
    if (ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &hp))
    {
        fprintf(stderr, "sound: %s won't play 48 kHz stereo: %s\n", path.c_str(), strerror(errno));
        close(pcm);
        pcm = -1;
        return false;
    }
    period = Interval(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    periods = Interval(&hp, SNDRV_PCM_HW_PARAM_PERIODS)->min;
    uint32_t buffer = period * periods;

    snd_pcm_sw_params sp;
    memset(&sp, 0, sizeof sp);
    sp.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
    sp.period_step = 1;
    sp.avail_min = period;
    sp.start_threshold = buffer;
    sp.stop_threshold = buffer;
    sp.xfer_align = period / 2;
    sp.boundary = buffer;
    while (sp.boundary * 2 <= (snd_pcm_uframes_t)LONG_MAX - buffer) sp.boundary *= 2;
    if (ioctl(pcm, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) || ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE))
    {
        fprintf(stderr, "sound: setting up %s: %s\n", path.c_str(), strerror(errno));
        close(pcm);
        pcm = -1;
        return false;
    }
    fprintf(stderr, "sound: %s, 48 kHz stereo, %u x %u frames\n", path.c_str(), periods, period);
    return true;
}

bool SoundOut::Write(const int16_t* data, uint32_t frames)
{
    if (wav)
    {
        // a sound card's pace, for tests
        auto start = wavStart;
        if (!wavFrames) start = wavStart = std::chrono::steady_clock::now();
        std::this_thread::sleep_until(start + std::chrono::microseconds(1000000ull * (wavFrames + frames) / Rate));
        fwrite(data, 4, frames, wav);
        wavFrames += frames;
        return true;
    }
    snd_xferi x {};
    x.buf = (void*)data;
    x.frames = frames;
    while (ioctl(pcm, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0)
    {
        if (errno == EINTR) continue;
        if (errno == EPIPE)
        {
            // the card ran dry: start it again
            xruns++;
            ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE);
            continue;
        }
        fprintf(stderr, "sound: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static void WavHeader(FILE* f, uint32_t frames)
{
    uint32_t bytes = frames * 4;
    uint8_t h[44];
    auto u32 = [&](int at, uint32_t v) { memcpy(h + at, &v, 4); };
    auto u16 = [&](int at, uint16_t v) { memcpy(h + at, &v, 2); };
    memcpy(h, "RIFF", 4); u32(4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8);
    u32(16, 16); u16(20, 1); u16(22, 2); u32(24, SoundOut::Rate); u32(28, SoundOut::Rate * 4); u16(32, 4); u16(34, 16);
    memcpy(h + 36, "data", 4); u32(40, bytes);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

bool SoundOut::Open(const std::string& spec)
{
    if (spec.rfind("wav:", 0) == 0)
    {
        wav = fopen(spec.c_str() + 4, "wb");
        if (!wav) { perror(spec.c_str() + 4); return false; }
        WavHeader(wav, 0);
    }
    else if (spec == "alsa")
    {
        std::string path = PcmPath();
        if (path.empty()) { fprintf(stderr, "sound: no sound card found (AUDIODEV=%s)\n", getenv("AUDIODEV") ? getenv("AUDIODEV") : ""); return false; }
        if (!OpenPcm(path)) return false;
    }
    else
    {
        fprintf(stderr, "sound: unknown output %s (alsa or wav:FILE)\n", spec.c_str());
        return false;
    }
    ring.assign(RingFrames * 2, 0);
    thread = std::thread([this] { Run(); });
    return true;
}

void SoundOut::Push(const int16_t* data, uint32_t frames)
{
    stretched.clear();
    stretcher.Process(data, frames, stretchBy, stretched);
    data = stretched.data();
    frames = (uint32_t)(stretched.size() / 2);
    std::lock_guard<std::mutex> g(lock);
    for (uint32_t i = 0; i < frames; i++)
    {
        if (head - tail >= RingFrames - 1) tail++;   // full: the oldest goes
        size_t at = (head % RingFrames) * 2;
        ring[at] = data[i * 2];
        ring[at + 1] = data[i * 2 + 1];
        head++;
    }
    more.notify_one();
}

double SoundOut::Skew(double speed)
{
    double fill;
    {
        std::lock_guard<std::mutex> g(lock);
        fill = (double)(head - tail);
    }
    fillAvg = fillAvg < 0 ? fill : fillAvg * 0.95 + fill * 0.05;
    static const bool log = getenv("MIRROR_SOUNDLOG") != nullptr;
    static uint32_t calls;
    if (log && ++calls % 600 == 0)
        fprintf(stderr, "sound: %.0f frames waiting, %u underruns\n", fillAvg, (unsigned)underruns);
    // 1% faster or slower at most: the game runs at the TV's 60 Hz, 0.3% off the DS's 59.83.
    // The slow part (drift) takes that 0.3% over in a few seconds, so the buffer settles at
    // its target instead of 30% above it.
    double e = (fillAvg - TargetFill) / TargetFill;
    drift = std::max(-0.01, std::min(0.01, drift + e * 0.00002));
    double trim = 1.0 + std::max(-0.01, std::min(0.01, e * 0.01 + drift));
    stretchBy = 1 / std::max(0.85, std::min(1.0, speed));
    return trim;
}

void SoundOut::Run()
{
    pthread_setname_np(pthread_self(), "mirror-sound");
    std::vector<int16_t> buf;
    bool started = false;
    while (!stop)
    {
        buf.assign(period * 2, 0);
        uint32_t got = 0;
        {
            std::unique_lock<std::mutex> g(lock);
            // start once there's a buffer's worth, so it doesn't run dry straight away
            if (!started && head - tail >= TargetFill) started = true;
            // Short of a period: the card still has most of its buffer to play (16 ms), so
            // give a slow frame (a course loading, a big bundle) that long before silence.
            if (started && head - tail < period)
                more.wait_for(g, std::chrono::milliseconds(12), [&] { return head - tail >= period || stop; });
            size_t have = head - tail;
            if (started)
            {
                got = (uint32_t)std::min<size_t>(have, period);
                for (uint32_t i = 0; i < got; i++)
                {
                    size_t at = (tail % RingFrames) * 2;
                    buf[i * 2] = ring[at];
                    buf[i * 2 + 1] = ring[at + 1];
                    tail++;
                }
                if (got < period)
                {
                    underruns++;
                    started = false;   // let it fill up again
                }
            }
        }
        if (!Write(buf.data(), period)) break;
    }
}

void SoundOut::Close()
{
    stop = true;
    if (thread.joinable()) thread.join();
    if (pcm >= 0)
    {
        ioctl(pcm, SNDRV_PCM_IOCTL_DROP);
        close(pcm);
        pcm = -1;
    }
    if (wav)
    {
        WavHeader(wav, wavFrames);
        fclose(wav);
        wav = nullptr;
    }
}
