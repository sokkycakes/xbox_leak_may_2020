// The game on the Sion's TV: both screens, in the mode the TV is already in.
//   drm         the Sion's display (KMS dumb buffers, as xbox-av does), taken over from
//               xbox-av the way a title does ('T' first, 'S' and the last frame when done)
//   dump:FILE   the composed pictures, one in 60 (MIRROR_TVDUMPEVERY) from the first
//               (MIRROR_TVDUMPFROM), for tests where there is no display
#ifndef MIRROR_TVOUT_H
#define MIRROR_TVOUT_H

#include <cstdint>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// where the screens go (the game menu's Screen layout): one of them alone, one above the
// other, side by side, or one big with the other at half its size beside it
enum class TVLayout { TopOnly, BottomOnly, Stacked, SideBySide, BigTop, BigBottom, Count };
inline bool SingleScreen(TVLayout l) { return l == TVLayout::TopOnly || l == TVLayout::BottomOnly; }

class TVOut
{
public:
    ~TVOut() { Close(); }
    bool Open(const std::string& spec);
    // Puts the two 256x192 screens (XRGB8888) on the TV. Waits for the frame before to
    // have reached the screen first, so the game runs at the TV's pace (Paced()).
    void Show(const uint32_t* top, const uint32_t* bottom);
    bool Paced() const { return paced; }
    // the TV's frame rate (the mode's pixel clock over its frame size)
    double Hz() const;
    void Close();
    // swap: the bottom screen where the top one goes and the other way round (not with
    // one screen alone)
    void SetLayout(TVLayout layout, bool swap);
    // drawn over each picture once it's composed (the game menu): the frame, its pitch in
    // bytes, its size and a pixel's width over its height
    using Overlay = std::function<void(uint32_t* dst, uint32_t pitch, uint32_t w, uint32_t h, double par)>;
    void SetOverlay(Overlay o);

private:
    void Layout();
    void Compose(uint32_t* dst, uint32_t pitch, const uint32_t* top, const uint32_t* bottom);
    // the screens and the overlay into a dst w pixels wide (the TV's, or the laid-out 720)
    void Draw(uint32_t* dst, uint32_t pitch, uint32_t w, const uint32_t* top, const uint32_t* bottom);
    bool WaitFlip(int ms);

    uint32_t w = 720, h = 480;
    double par = 1;                    // a pixel's width over its height
    bool paced = false;
    TVLayout layout = TVLayout::TopOnly;
    bool swap = false;
    // where each screen goes: the top one first
    struct Place
    {
        uint32_t x = 0, y = 0, w = 0, h = 0;
        std::vector<uint32_t> xs, ys;  // per output column/row: source index << 9 | weight (of 256)
    } place[2];
    std::vector<uint32_t> rowCache;    // horizontally scaled source rows
    uint32_t rep = 1;                  // TV pixels per laid-out pixel (4 at 2880x480i)
    std::vector<uint32_t> narrow;      // the picture laid out 720 wide, when rep > 1
    Overlay overlay;
    int clearFrames = 0;               // pictures still to clear whole (the layout or overlay changed)

    // dump: (MIRROR_TVPACE=1: paced by a pretend 60 Hz TV, for cloud tests)
    FILE* dump = nullptr;
    double vblankPeriod = 0, flipAt = 0;   // seconds on the steady clock
    uint32_t frames = 0, dumpEvery = 60, dumpFrom = 0;
    std::vector<uint32_t> dumpBuf;

    // drm:
    int drm = -1, av = -1;
    uint32_t connId = 0, crtcId = 0;
    struct Buf { uint32_t fb, handle, pitch; uint64_t size; uint8_t* map; } bufs[2] {};
    int front = -1;
    bool flipPending = false, crtcSet = false;
    uint32_t flipTimeouts = 0;
    std::vector<uint8_t> modeInfo;     // struct drm_mode_modeinfo
};

#endif
