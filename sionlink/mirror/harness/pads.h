// The Sion's own pads playing the game (--pads 1): any pad SDL knows, through SDL's
// game controller layer (its HIDAPI drivers over libusb as xbcompat's SDL does, the
// kernel's evdev otherwise), with gamecontrollerdb.txt for the odd ones.
//
// Buttons go by where they sit, as on the DS: the pad's right face button is the DS's A,
// the bottom one B, the top one X, the left one Y. Back is Select, the shoulders L and R,
// the left stick or the d-pad the d-pad. The right stick moves a stylus over the bottom
// screen and the right trigger (or a click of the right stick) touches there. The Guide
// button stays the Guide's. Holding Back for 2 s asks for the game's menu (MenuAsked):
// until then Back is Select for the game, from then on it's let go.
#ifndef MIRROR_PADS_H
#define MIRROR_PADS_H

#include <cstdint>
#include <vector>

class SionPads
{
public:
    ~SionPads() { Close(); }
    bool Open();
    void Close();
    // once a frame: held is the DS's buttons (libnds order, bit set = pressed)
    void Poll(uint16_t& held, bool& touch, uint16_t& x, uint16_t& y);
    // the stylus, while a pad has moved it lately (for the TV to draw)
    bool CursorShown() const { return cursorFrames > 0; }
    int CursorX() const { return (int)cx; }
    int CursorY() const { return (int)cy; }
    // Back held for 2 s since the last call
    bool MenuAsked() { bool m = menuAsked; menuAsked = false; return m; }

private:
    bool open = false;
    std::vector<void*> pads;   // SDL_GameController*
    float cx = 128, cy = 96;
    int cursorFrames = 0;
    uint32_t backFrames = 0;
    bool backSpent = false, menuAsked = false;
};

// draws the stylus on a copy of the bottom screen (256x192 XRGB)
void DrawCursor(uint32_t* screen, int x, int y, bool touching);

#endif
