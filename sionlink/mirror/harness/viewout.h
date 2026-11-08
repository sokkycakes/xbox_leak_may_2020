// --view: the mirror on a handheld with two screens of its own (the Anbernic RG DS under
// BananaOS first). mirrorrun runs the DS client (the same .nds a real DS runs) with a
// simulated DSpico whose USB goes over the network to the Sion (--dspico tcp:HOST:PORT),
// and this shows the client's two screens and gives it the handheld's pad and touch screen.
//
//   sdl          one SDL window, the top screen above the bottom one. On BananaOS run with
//                SDL_VIDEODRIVER=dummy SDL_BANANA_PANELS=stack: SDL's window then covers
//                both 640x480 panels, so each DS screen fills one panel (2.5x), and the
//                bottom panel's touchscreen lands on the bottom half. Elsewhere it's an
//                ordinary window (a desktop, for tests).
//   dump:FILE    the composed pictures (MIRROR_TVDUMPEVERY, default 60), for cloud tests
//
// Buttons go by where they sit, as on the DS (pads.h): the pad's right face button is A,
// the bottom one B, the top X, the left Y; Back is Select. The Guide button (or Escape)
// quits.
#ifndef MIRROR_VIEWOUT_H
#define MIRROR_VIEWOUT_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class ViewOut
{
public:
    ~ViewOut() { Close(); }
    bool Open(const std::string& spec);
    void Show(const uint32_t* top, const uint32_t* bottom);
    // once a frame: the DS's buttons (libnds order, bit set = held) and the stylus
    void Poll(uint16_t& held, bool& touch, uint16_t& x, uint16_t& y);
    bool QuitAsked() const { return quit; }
    void Close();

private:
    void Compose(uint32_t* dst, uint32_t pitch, const uint32_t* top, const uint32_t* bottom);
    void Scale();

    uint32_t w = 640, h = 960;
    std::vector<uint32_t> xs, ys;     // per output column/row of one screen: the source pixel
    uint32_t sw = 0, sh = 0, sx0 = 0, sy0[2] {};   // one screen's size and place
    bool quit = false;
    void* window = nullptr;           // SDL_Window*
    std::vector<void*> pads;          // SDL_GameController*
    uint16_t held = 0;
    bool touching = false;
    uint16_t tx = 0, ty = 0;
    FILE* dump = nullptr;
    std::vector<uint32_t> dumpBuf;
    uint32_t frames = 0, dumpEvery = 60;
};

#endif
