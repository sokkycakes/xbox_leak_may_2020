// The app's own screens, in the NitroLink look (sokky's "DS UI" Figma, art/): its art on the
// top screen, the backdrop on the lower one with text in Tahoma at its own 10 pixels. They
// take both screens from the mirrored game; the mirror clears the hardware before the game
// is shown again (a full bundle), so nothing of them is left behind.
#ifndef NITROUI_H
#define NITROUI_H

#include <nds.h>

#include "mirror_format.h"

// both screens ours: the art on top, the backdrop below (main engine on top, bitmap BGs in
// VRAM banks A and C)
void uiShow(void);
bool uiShown(void);
// the mirror's again (it clears VRAM and sets every register)
void uiLeave(void);
// the lower screen: a line or two in the middle (line2 may be NULL)
void uiMessage(const char* line1, const char* line2);
// the same, with a progress bar under them (percent >= 0) and a small grey line at the foot
// (foot may be NULL)
void uiStatus(const char* line1, const char* line2, int percent, const char* foot);
// the lower screen: the game's menu (MREC_MENU: the record's payload, len bytes)
void uiMenu(const mirror_menu* m, u32 len);

// called while the art unpacks (main.c: keeps USB going)
void uiPoll(void);

#endif
