// The game's menu on the TV, as Game Boy Player and Virtual Console have one: holding
// Select for 2 s (Back on the Sion's pads, or the DS's own Select over the link) stops the
// game and opens it over the dimmed picture. It has the screen layout, quick save and
// load, reset, and the way back to the dashboard; its header shows the game and how the
// DS link is doing. Frolick's look: its slate panel, white text, the focused row light
// with dark text, the Blades' A and B buttons in the legend (menuart.h).
#ifndef MIRROR_MENU_H
#define MIRROR_MENU_H

#include <cstdint>
#include <memory>
#include <string>

#include "tvout.h"

// the menu's keys, from the Sion's pads (SionPads::MenuKeys) or the DS's
enum MenuKey : uint16_t { MK_UP = 1, MK_DOWN = 2, MK_LEFT = 4, MK_RIGHT = 8, MK_OK = 16, MK_BACK = 32, MK_CLOSE = 64 };

// what to do once the menu has closed
enum class MenuAction { None, Resume, QuickSave, QuickLoad, Reset, Dashboard };

class GameMenu
{
public:
    GameMenu();
    ~GameMenu();

    // the game, from its ROM's banner: its English title and its icon
    void TakeBanner(const uint8_t* rom, uint32_t len);
    const std::string& Title() const { return title; }

    // what the menu shows; the layout rows change these, and the TV follows
    std::string link;          // the DS link's state, a few words
    bool linked = false;
    std::string stateWhen;     // when the quick save was made (empty: none)
    TVLayout layout = TVLayout::Stacked;
    bool swap = false;

    void Open();
    bool IsOpen() const { return open; }
    // once a TV frame while it's open, with the keys held then: None while it stays open
    MenuAction Update(uint16_t keys);
    // a line at the foot of the TV for a moment, over the game (after a quick save)
    void Toast(const std::string& text);
    bool Toasting() const { return toastFrames > 0; }
    // once a frame of the game's: the toast's time
    void Tick() { if (toastFrames > 0) toastFrames--; }
    // TVOut's overlay: the open menu over the dimmed game, or the toast
    void Draw(uint32_t* dst, uint32_t pitch, uint32_t w, uint32_t h, double par);

    static const char* LayoutName(TVLayout l);

private:
    struct Art;
    std::unique_ptr<Art> art;  // the font and buttons at the TV's size and pixel shape
    std::string title;
    uint32_t icon[32 * 32] = {};
    bool hasIcon = false;

    bool open = false;
    int page = 0;              // 0: the menu; else the MenuAction waiting for a yes
    int focus = 0, confirmFocus = 0;
    uint16_t prev = 0, blocked = 0;
    int repeatIn[4] = {};
    std::string toast;
    int toastFrames = 0;
};

#endif
