// The Sion's own pads playing the game (--pads 1): any pad SDL knows, through SDL's
// game controller layer (its HIDAPI drivers over libusb as xbcompat's SDL does, the
// kernel's evdev otherwise), with gamecontrollerdb.txt for the odd ones.
//
// Buttons go by where they sit, as on the DS: the pad's right face button is the DS's A,
// the bottom one B, the top one X, the left one Y. Back is Select, the shoulders L and R,
// the left stick or the d-pad the d-pad. The right stick moves a stylus over the bottom
// screen and the right trigger (or a click of the right stick) touches there. The Guide
// button stays the Guide's. Holding Back for 2 s asks for the game's menu (MenuAsked):
// until then Back is Select for the game, from then on it's let go. SDL runs on a thread of
// its own ("pads"): a pad coming or going (run 16: 142 ms) doesn't hold the game up.
#ifndef MIRROR_PADS_H
#define MIRROR_PADS_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
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
    // what the pads have pressed (buttons since the game last looked, too) and the right stick
    struct State { uint16_t held = 0; bool back = false, touch = false; float sx = 0, sy = 0; };
    void Read(State& st);
    bool Init();

    bool open = false, threaded = false;
    std::thread thread;
    std::mutex lock;
    std::atomic<bool> stop {false};
    State latest, pressed;     // pressed: held at any poll since the game's last Poll
    std::vector<void*> pads;   // SDL_GameController*
    float cx = 128, cy = 96;
    int cursorFrames = 0;
    uint32_t backFrames = 0;
    bool backSpent = false, menuAsked = false;
};

// draws the stylus on a copy of the bottom screen (256x192 XRGB)
void DrawCursor(uint32_t* screen, int x, int y, bool touching);

#endif
