// The game's sound on the Sion, 48 kHz stereo:
//   alsa        the ALSA device the dashboard uses ($AUDIODEV, e.g. hdmi:CARD=PCH,DEV=0
//               = the TV's HDMI), straight through the kernel's PCM interface
//   wav:FILE    a WAV file, for tests where there is no sound card
// The game's samples go into a buffer that a thread plays from. How full it is steers
// how fast melonDS makes samples (SPU::SetOutputSkew), since the game runs at the TV's
// pace, not exactly the DS's.
#ifndef MIRROR_SOUNDOUT_H
#define MIRROR_SOUNDOUT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stretch.h"

class SoundOut
{
public:
    static constexpr int Rate = 48000;
    ~SoundOut() { Close(); }
    bool Open(const std::string& spec);
    // stereo frames from the game
    void Push(const int16_t* data, uint32_t frames);
    // How fast the game should make samples, relative to normal (within 1%): above 1 when
    // the buffer runs full. speed: how fast the game runs against the DS (below 1 in a scene
    // too heavy for the Sion); the sound pushed next is then made longer to match (up to
    // 18%, at the same pitch: stretch.h) instead of running dry.
    double Skew(double speed = 1);
    // the game stands still (its menu is up): silence, and not counted as running dry;
    // what was waiting plays once it goes on
    void Pause(bool p) { paused = p; more.notify_one(); }
    void Close();
    // times the buffer ran dry (the game was late), and times the card did (this thread was)
    uint32_t Underruns() const { return underruns; }
    uint32_t Xruns() const { return xruns; }

private:
    void Run();
    bool OpenPcm(const std::string& path);
    bool Write(const int16_t* data, uint32_t frames);

    int pcm = -1;
    FILE* wav = nullptr;
    uint32_t wavFrames = 0;
    std::chrono::steady_clock::time_point wavStart;
    uint32_t period = 256, periods = 4;
    std::vector<int16_t> ring;       // stereo frames waiting to be played
    size_t head = 0, tail = 0;       // in frames
    std::mutex lock;
    std::thread thread;
    std::atomic<bool> stop {false}, paused {false};
    std::atomic<uint32_t> underruns {0}, xruns {0};
    std::condition_variable more;    // the game pushed samples
    double fillAvg = -1, drift = 0;
    Stretcher stretcher;
    double stretchBy = 1;
    std::vector<int16_t> stretched;
};

#endif
