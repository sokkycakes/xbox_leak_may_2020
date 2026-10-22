// The game on the Sion's TV: both screens, stacked, in the mode the TV is already in.
//   drm         the Sion's display (KMS dumb buffers, as xbox-av does), taken over from
//               xbox-av the way a title does ('T' first, 'S' and the last frame when done)
//   dump:FILE   the composed pictures, one in 60, for tests where there is no display
#ifndef MIRROR_TVOUT_H
#define MIRROR_TVOUT_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class TVOut
{
public:
    ~TVOut() { Close(); }
    bool Open(const std::string& spec);
    // Puts the two 256x192 screens (XRGB8888) on the TV. Waits for the frame before to
    // have reached the screen first, so the game runs at the TV's pace (Paced()).
    void Show(const uint32_t* top, const uint32_t* bottom);
    bool Paced() const { return paced; }
    void Close();

private:
    void Layout();
    void Compose(uint32_t* dst, uint32_t pitch, const uint32_t* top, const uint32_t* bottom);
    bool WaitFlip(int ms);

    uint32_t w = 720, h = 480;
    bool paced = false;
    // layout: each screen sw x sh, the top one at (x0, y0), the bottom one under it
    uint32_t sw = 0, sh = 0, x0 = 0, y0 = 0;
    std::vector<uint32_t> xs, ys;      // per output column/row: source index << 9 | weight (of 256)
    std::vector<uint32_t> rowCache;    // horizontally scaled source rows

    // dump:
    FILE* dump = nullptr;
    uint32_t frames = 0;
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
