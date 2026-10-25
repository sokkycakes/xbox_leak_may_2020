// Sion Link for the DS: the live mirror of a DS game running on the Sion (bundles over the
// DSpico's USB), its home screen with the Sion's Wi-Fi link (Download Play, wifilink.c) and
// the DS as the Sion's pad while no game is mirrored. With nitro:/mirror.bin (or one on the
// SD card) it replays that recording of what an emulated game did to its display hardware
// instead.
//
// Per frame: during the visible lines push the bundle's 3D writes (their
// SWAP_BUFFERS lands at the next VBlank, as it did in the emulator), then in
// VBlank apply its 2D state. Both show up together in the next frame.

#include <nds.h>
#include <fat.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mirror_format.h"
// a big bundle takes a while to unpack: keep the USB transfers going
static void lzTick(void);
#define MIRROR_LZ_TICK() lzTick()
#include "mirror_lz.h"
#include "nitroui.h"
#include "slboot.h"
#include "usblink.h"
#include "wifilink.h"

static const u32 bankLCDC[9] = {
    0x06800000, 0x06820000, 0x06840000, 0x06860000,
    0x06880000, 0x06890000, 0x06894000, 0x06898000, 0x068A0000,
};
static const u32 bankSize[9] = {
    0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000,
};

static vu8* vramcnt(int bank)
{
    return (vu8*)(0x04000240 + (bank < 7 ? bank : bank + 1));
}

static u8 curMap[9];

static void copyWords(u32 dst, const void* src, u32 bytes)
{
    vu32* d = (vu32*)dst;
    const u32* s = (const u32*)src;
    for (u32 i = 0; i < bytes / 4; i++)
        d[i] = s[i];
}

// Iterate records; returns NULL at MREC_END.
static const mirror_rec* nextRec(const mirror_rec* r)
{
    return (const mirror_rec*)((const u8*)(r + 1) + r->len);
}

// The registers the Sion mirrors (harness/mirrorrun.cpp: Tracked2D, Tracked3D, and
// POWCNT). Nothing in a bundle may write any other: one stray write to the card
// registers can send the DSpico a command it doesn't know, and it stops answering
// until the DS is switched off.
static bool tracked2D(u32 o)
{
    if (o < 0x58)
        return o >= 8 || o < 4;   // DISPCNT, BG, affine, window, blend; not DISPSTAT/VCOUNT
    if (o >= 0x64 && o < 0x68)
        return true;               // DISPCAPCNT
    if (o == 0x6C || o == 0x6D)
        return true;               // MASTER_BRIGHT
    if (o >= 0x1000 && o < 0x1058)
        return o >= 0x1008 || o < 0x1004;
    return o == 0x106C || o == 0x106D;
}

static bool tracked3D(u32 o)
{
    if (o >= 0x60 && o < 0x64)
        return true;               // DISP3DCNT
    return o >= 0x320 && o < 0x6A4 && !(o >= 0x600 && o < 0x608);   // not GXSTAT
}

// a write of `size` bytes at 0x04000000 + off; 2D state may also write the 3D registers
static bool ioOk(u32 off, u32 size, bool twoD)
{
    if (off & (size - 1))
        return false;
    for (u32 o = off; o < off + size; o++)
        if (!(o == 0x304 || o == 0x305 || tracked3D(o) || (twoD && tracked2D(o))))
            return false;
    return true;
}

// The 3D commands of the last bundle read, as an MREC_GX payload. A delta
// record copies from the one before.
static u32* gxPayload;
static u32 gxWords;

// every run of a payload goes to a 3D register and fits
static bool gxRunsOk(const u32* p, u32 words)
{
    if (!words)
        return true;
    const u32* end = p + words;
    u32 runs = *p++;
    for (u32 i = 0; i < runs; i++)
    {
        if (p >= end)
            return false;
        const mirror_run* run = (const mirror_run*)p++;
        u32 n = run->info & 0x3FFF, code = run->info >> 14;
        if (code > 2 || n > (u32)(end - p) || !ioOk(run->off, code == 0 ? 4 : code == 1 ? 2 : 1, false))
            return false;
        p += n;
    }
    return true;
}

enum { GX_OK, GX_DAMAGED, GX_NO_MEMORY };

// A damaged delta (or one whose base this DS never had) gives no 3D this frame, and
// the caller asks for a full bundle: the next deltas would build on what's missing.
static int decodeGX(const mirror_hdr* h)
{
    u32* prev = gxPayload;
    u32 prevWords = gxWords;
    int result = GX_OK;
    gxPayload = NULL;
    gxWords = 0;
    for (const mirror_rec* r = (const mirror_rec*)(h + 1); r->type != MREC_END; r = nextRec(r))
    {
        const u32* p = (const u32*)(r + 1);
        if (r->type == MREC_GX)
        {
            gxPayload = malloc(r->len);
            if (!gxPayload)
            {
                result = GX_NO_MEMORY;
                break;
            }
            gxWords = r->len / 4;
            // in 16 KB pieces: a course's whole 3D scene is over 100 KB
            for (u32 at = 0; at < r->len; at += 16384)
            {
                memcpy((u8*)gxPayload + at, (const u8*)p + at, r->len - at < 16384 ? r->len - at : 16384);
                lzTick();
            }
        }
        else if (r->type == MREC_GXDELTA)
        {
            u32 n = *p++, i = 0;
            if (n > 0x80000)
            {
                result = GX_DAMAGED;
                break;
            }
            gxPayload = malloc(n * 4);
            if (!gxPayload)
            {
                result = GX_NO_MEMORY;
                break;
            }
            gxWords = n;
            const u32* recEnd = (const u32*)((const u8*)p - 4 + r->len);
            while (i < n)
            {
                u32 op = p < recEnd ? *p++ : 0;
                u32 len = op & 0x7FFFFFFF;
                if (!len || len > n - i)
                {
                    result = GX_DAMAGED;
                    break;
                }
                if (op & 0x80000000)
                {
                    u32 src = p < recEnd ? *p++ : ~0u;
                    if (!prev || src > prevWords || len > prevWords - src)
                    {
                        result = GX_DAMAGED;
                        break;
                    }
                    memcpy(gxPayload + i, prev + src, len * 4);
                }
                else
                {
                    if (len > (u32)(recEnd - p))
                    {
                        result = GX_DAMAGED;
                        break;
                    }
                    memcpy(gxPayload + i, p, len * 4);
                    p += len;
                }
                if (((i + len) ^ i) >> 12)
                    lzTick();   // every 16 KB or so
                i += len;
            }
        }
    }
    if (result == GX_OK && !gxRunsOk(gxPayload, gxWords))
        result = GX_DAMAGED;
    if (result != GX_OK)
    {
        free(gxPayload);
        gxPayload = NULL;
        gxWords = 0;
    }
    free(prev);
    return result;
}

static bool liveLink;

static void pushGX(void)
{
    if (!gxPayload || !gxWords)
        return;
    const u32* p = gxPayload;
    const u32* end = gxPayload + gxWords;
    u32 runs = *p++;
    for (u32 i = 0; i < runs && p < end; i++)
    {
        const mirror_run* run = (const mirror_run*)p++;
        u32 addr = 0x04000000 + run->off;
        u32 n = run->info & 0x3FFF, code = run->info >> 14;
        if (p + n > end)
            break;
        if (run->off == 0x304)
        {
            // power control, in order with the 3D writes; screen swap stays as the 2D state set it
            for (u32 k = 0; k < n; k++)
                REG_POWERCNT = (*p++ & 0x7FFF) | (REG_POWERCNT & 0x8000);
        }
        else if (code == 0)
        {
            // a dense frame can keep the geometry FIFO full for milliseconds:
            // keep taking USB data meanwhile
            for (u32 k = 0; k < n; k++)
            {
                *(vu32*)addr = *p++;
                if (liveLink && (k & 255) == 255)
                    usbPoll();
            }
        }
        else if (code == 1)
            for (u32 k = 0; k < n; k++) *(vu16*)addr = *p++;
        else
            for (u32 k = 0; k < n; k++) *(vu8*)addr = *p++;
    }
}

// Register changes partway down the screen, replayed from the HBlank IRQ.
static const mirror_line* lineList;
static u32 lineCount, lineNext;

static void hblankHandler(void)
{
    u32 target = REG_VCOUNT + 1;
    if (target >= 192)
        return;
    // lines already passed (this IRQ came late) are still written: later frames
    // only send what changed from the end-of-frame values
    while (lineNext < lineCount && lineList[lineNext].line <= target)
    {
        const mirror_line* l = &lineList[lineNext++];
        *(vu16*)(0x04000000 + l->off) = l->val;
    }
}

// finish the current frame's line changes, so the registers hold its end-of-frame values
static void flushLines(void)
{
    while (lineNext < lineCount)
    {
        const mirror_line* l = &lineList[lineNext++];
        *(vu16*)(0x04000000 + l->off) = l->val;
    }
}

static void apply2D(const mirror_hdr* h)
{
    const mirror_rec* r;
    const u8* newMap = NULL;

    // VRAM first: put each bank we touch in LCDC mode so the CPU can write it
    for (r = (const mirror_rec*)(h + 1); r->type != MREC_END; r = nextRec(r))
    {
        if (r->type == MREC_VRAMCNT)
            newMap = (const u8*)(r + 1);
        if (r->type != MREC_VRAM)
            continue;
        const u32* p = (const u32*)(r + 1);
        u32 n = *p++;
        u32 inLCDC = 0;
        for (u32 i = 0; i < n; i++, p += 1 + MIRROR_BLOCK / 4)
        {
            u32 bank = p[0] >> 16, blk = p[0] & 0xFFFF;
            if (!(inLCDC & (1 << bank)))
            {
                *vramcnt(bank) = 0x80;
                inLCDC |= 1 << bank;
            }
            copyWords(bankLCDC[bank] + blk * MIRROR_BLOCK, p + 1, MIRROR_BLOCK);
        }
        for (int b = 0; b < 9; b++)
            if (inLCDC & (1 << b))
                *vramcnt(b) = curMap[b];
    }
    if (newMap)
    {
        for (int b = 0; b < 9; b++)
        {
            curMap[b] = newMap[b];
            *vramcnt(b) = newMap[b];
        }
    }

    for (r = (const mirror_rec*)(h + 1); r->type != MREC_END; r = nextRec(r))
    {
        const u32* p = (const u32*)(r + 1);
        u32 n = p[0];
        if (r->type == MREC_PAL || r->type == MREC_OAM)
        {
            u32 base = r->type == MREC_PAL ? 0x05000000 : 0x07000000;
            p++;
            for (u32 i = 0; i < n; i++, p += 1 + MIRROR_ROW / 4)
                copyWords(base + p[0], p + 1, MIRROR_ROW);
        }
        else if (r->type == MREC_REGS)
        {
            const mirror_reg* reg = (const mirror_reg*)(p + 1);
            for (u32 i = 0; i < n; i++)
                *(vu16*)(0x04000000 + reg[i].off) = reg[i].val;
        }
    }

    lineList = NULL;
    lineCount = lineNext = 0;
    for (r = (const mirror_rec*)(h + 1); r->type != MREC_END; r = nextRec(r))
    {
        if (r->type == MREC_LINES)
        {
            lineCount = *(const u32*)(r + 1);
            lineList = (const mirror_line*)((const u32*)(r + 1) + 1);
        }
    }
    if (lineCount)
        irqEnable(IRQ_HBLANK);
    else
        irqDisable(IRQ_HBLANK);
}

// Bundles come from a recording (file) or live from the Sion over the
// DSpico's USB port (live link).
extern volatile u32 mirrorStats[4];
static FILE* recording;
static u32 vblanks, lastScan, lastReport;
static u32 lastKeys, lastTouch, lastFlags, lastShown;   // what the last report said
static volatile u32 shownFrame;   // frame number of the bundle on screen (vblankHandler)

static void clearHardware(void);
static bool linkData;     // the Sion has started sending
// Live: a game is mirrored (from its first full bundle). Otherwise the app's own screens
// are up (home), the Wi-Fi link runs, and the Sion may take the DS as its pad.
static bool mirroring;
static bool leaveMirror;  // the mirror ends: readBundleLive gives up, the main loop goes home
static bool padOn;        // the Sion's pad service has the DS (MREC_PAD)
static u32 padSeenAt;
static char padText[2][64];
// Live: the screens no longer match the stream (a bundle was lost, or the
// stall screen took over); bundles are dropped until the Sion sends a full one
static bool needFull = true;
static bool stallShown, stallManual;
static bool menuUp;      // the game's menu is up on the TV, and here on the lower screen
static bool diagShown;   // before the Sion sends anything: the link's details instead of the art
static u32 comboSince;   // VBlank count when L+R+SELECT went down, 0 while it isn't held
static bool comboDone;
static u32 lastApplied, lastAppliedAt;
static u32 lastOutBytes, lastDataAt;   // when data last arrived from the Sion
static u32 resyncs, allocFails, fullBundles, stalls;
static void showStall(void);
static void showStallScreen(void);
static u32 lastDiag = ~0u;

// Before the Sion sends anything, L+R+SELECT shows what the link has seen
static void showDiag(void)
{
    if (!diagShown || vblanks - lastDiag < 15)
        return;
    lastDiag = vblanks;
    printf("\x1b[0;0HSion Link: waiting  %5lu\n", vblanks / 60);
    printf("card id %08lX key2 %08lX\n", usbDiag.idPlain, usbDiag.idKey2);
    printf("irqs %lu  events %lu\n", usbDiag.irqs, usbDiag.events);
    printf("resets %lu  setups %lu\n", usbDiag.resets, usbDiag.setups);
    printf("state %-14s\n\n", usbConfigured() ? "connected" : usbDiag.setups ? "enumerating" : "no host yet");
    u32 n = usbDiag.logCount;
    for (u32 k = 0; k < 8; k++)
    {
        if (k >= n) { printf("%-20s\n", ""); continue; }
        u32 i = (n - 1 - k) % 8;
        const typeof(usbDiag.log[0])* e = &usbDiag.log[i];
        if (e->kind == 'S')
            printf("setup %02X %04X len %-4u\n", e->x, e->a, e->b);
        else if (e->kind == 'C')
            printf("done  ep %02X %-3u bytes  \n", e->x, e->a);
        else if (e->kind == 'Z')
            printf("status sent         \n");
        else if (e->kind == 'N')
            printf("lost data recovered \n");
        else if (e->kind == 'A')
            printf("data started again  \n");
        else if (e->kind == 'I')
            printf("report started again\n");
        else
            printf("bus reset           \n");
    }
}

// The mirrored game may have left the sub engine faded to white or black,
// blended or forced blank: undo that so the status text can be read
static void showStallScreen(void)
{
    irqDisable(IRQ_HBLANK);
    lineList = NULL;
    lineCount = lineNext = 0;
    REG_POWERCNT |= POWER_2D_B | POWER_LCD;
    REG_MASTER_BRIGHT = 0;
    REG_MASTER_BRIGHT_SUB = 0;
    REG_BLDCNT_SUB = 0;
    REG_BLDY_SUB = 0;
    consoleDemoInit();
    REG_BG0HOFS_SUB = 0;
    REG_BG0VOFS_SUB = 0;
    BG_PALETTE_SUB[0] = RGB15(0, 0, 8);
    BG_PALETTE_SUB[255] = RGB15(31, 31, 31);
}

// Home: no game mirrored. The NitroLink look, saying what the links are doing: the DSpico's
// (the Sion's pad, or the DSpico gone quiet) and the Wi-Fi one (a Sion linked, Download Play).
static bool homeDrawn;   // what's on the lower screen is showHome's, and up to date

static void showHome(void)
{
    bool wlChanged;
    const wl_status* w = wlStatus(&wlChanged);
    static bool silentWas, padWas, usbWas;
    bool silent = usbSilent(), usb = usbLinked();
    if (homeDrawn && uiShown() && !wlChanged && silent == silentWas && padOn == padWas && usb == usbWas)
        return;
    homeDrawn = true;
    silentWas = silent;
    padWas = padOn;
    usbWas = usb;

    char a[96], b[96], foot[64];
    const char* l1 = "Waiting for link...";
    const char* l2 = NULL;
    int percent = -1;
    foot[0] = 0;
    if (usb)
        strcpy(foot, "USB: linked to the Sion (Wi-Fi off)");
    else if (w->state == WL_SEARCHING)
        strcpy(foot, "Wi-Fi: looking for a Sion");
    else if (w->state == WL_LINKING)
        snprintf(foot, sizeof foot, "Wi-Fi: linking to %s", w->host);
    else if (w->state == WL_LINKED)
        snprintf(foot, sizeof foot, "Wi-Fi: linked to %s", w->host);
    else if (w->state == WL_FAILED)
        strcpy(foot, "Wi-Fi didn't start");

    if (w->receiving || w->booting)
    {
        snprintf(a, sizeof a, "%s %s", w->booting ? "Starting" : "Receiving", w->name);
        l1 = a;
        if (!w->booting)
        {
            snprintf(b, sizeof b, "%lu%%", w->percent);
            l2 = b;
            percent = w->percent;
        }
    }
    else if (w->refused)
    {
        snprintf(a, sizeof a, "Couldn't start %s:", w->name);
        l1 = a;
        l2 = w->refused;
    }
    else if (silent)
    {
        l1 = "The DSpico stopped answering:";
        l2 = "switch the DS off and on.";
    }
    else if (padOn)
    {
        l1 = padText[0];
        l2 = padText[1][0] ? padText[1] : NULL;
    }
    else if (w->state == WL_LINKED)
    {
        snprintf(a, sizeof a, "Linked to %s", w->host);
        l1 = a;
        l2 = w->text[0] ? w->text : w->motd[0] ? w->motd : NULL;
        foot[0] = 0;
    }
    uiStatus(l1, l2, percent, foot);
}

// the mirror's memory is free again and its screens gone: the app's own (homeTick brings
// the Wi-Fi link back if the USB one is gone)
static void goHome(void)
{
    mirroring = leaveMirror = false;
    menuUp = stallShown = false;
    needFull = true;
    homeDrawn = false;
    showHome();
}

// a game's first full bundle: the Wi-Fi link stops if it was on (the mirror needs the DS's
// time and its memory), and so does being the Sion's pad
static void startMirror(void)
{
    mirroring = true;
    padOn = false;
    wlOff();
}

// Wi-Fi only while the DSpico's USB isn't linked to a Sion: off as soon as it is, on again
// 3 s after it goes (the cable out, the DSpico gone quiet), and at the start only after 3 s
// without it (a DSpico on a Sion links in about a second)
static u32 usbGoneAt;

static void wifiOrUsb(void)
{
    if (usbLinked())
    {
        usbGoneAt = 0;
        wlOff();
        return;
    }
    if (!usbGoneAt)
        usbGoneAt = vblanks ? vblanks : 1;
    else if (vblanks - usbGoneAt >= 3 * 60)
        wlOn();
}

static void homeTick(void)
{
    wifiOrUsb();
    wlTick();
    if (padOn && vblanks - padSeenAt > 3 * 60)
        padOn = false;   // the pad service went (it says so every second)
    if (!diagShown && !stallManual)
        showHome();
}

// L+R+SELECT: what the link and the stream are doing
static void showStall(void)
{
    if (vblanks - lastDiag < 15)
        return;
    lastDiag = vblanks;
    if (usbSilent())
        printf("\x1b[0;0HThe DSpico stopped answering.\nSwitch the DS off and on and\nstart Sion Link again.\n");
    else if (stallManual)
        printf("\x1b[0;0HSion Link: status            \n%-30s\n%-30s\n", "", "");
    else
        printf("\x1b[0;0HSion Link: picture stopped %4lu\n%-30s\n%-30s\n", (vblanks - lastAppliedAt) / 60, "", "");
    printf("frames shown  %lu\n", mirrorStats[1]);
    printf("full refresh  %lu\n", fullBundles);
    printf("resyncs       %lu\n", resyncs);
    printf("out of memory %lu\n", allocFails);
    printf("data waiting  %lu\n", usbAvailable());
    printf("usb events    %lu\n", usbDiag.events);
    printf("usb state     %-12s\n", usbSilent() ? "DSpico silent" : usbConfigured() ? "connected" : "lost");
    printf("silent reads  %lu\n", usbDiag.deadReads);
    printf("irq reads %lu, timed %lu\n", usbDiag.irqs, usbDiag.timedPolls);
    printf("nudges %lu, recovered %lu\n", usbDiag.nudges, usbDiag.recovered);
    printf("restarted: data %lu, pad %lu\n", usbDiag.rearmed, usbDiag.inRearmed);
    printf("out done %lu, %lu KB %s\n", usbDiag.outDone, usbDiag.outBytes >> 10, usbDiag.outArmed ? "armed" : "idle ");
    printf("in done %lu of %lu %s\n", usbDiag.inDone, usbDiag.reports, usbDiag.inBusy ? "busy" : "idle");
    printf("%-30s\n", stallManual ? "hold L+R+SELECT: back to game" : "hold L+R+SELECT: keep this");
    printf("waiting for a full frame: %s\n", needFull ? "yes" : "no ");
}

// live link: keep USB going and send the pad state once a frame
static void idle(void)
{
    if (!liveLink)
        return;
    usbPoll();
    // L+R+SELECT held for a second: show or hide the status page (before the Sion sends
    // anything, the link's details)
    if ((keysHeld() & (KEY_L | KEY_R | KEY_SELECT)) == (KEY_L | KEY_R | KEY_SELECT))
    {
        if (!comboSince)
            comboSince = vblanks | 1;
        if (!comboDone && vblanks - comboSince >= 60 && !linkData)
        {
            comboDone = true;
            diagShown = !diagShown;
            if (diagShown)
            {
                uiLeave();
                showStallScreen();
                lastDiag = ~0u - 15;
            }
            else
                homeDrawn = false;
        }
        else if (!comboDone && vblanks - comboSince >= 60)
        {
            comboDone = true;
            stallManual = !stallManual;
            menuUp = false;
            if (stallManual)
            {
                // the game's frames would draw over the page: drop them until it closes
                stallShown = true;
                needFull = true;
                uiLeave();
                showStallScreen();
            }
            else
            {
                // back to the game: a full frame repaints everything (home: its own screen)
                stallShown = false;
                needFull = true;
                homeDrawn = false;
                lastAppliedAt = vblanks;   // not a stall: give the full frame time to come
            }
        }
    }
    else
        comboSince = 0, comboDone = false;
    if (stallManual)
        showStall();
    if (usbDiag.outBytes != lastOutBytes)
    {
        lastOutBytes = usbDiag.outBytes;
        lastDataAt = vblanks;
    }
    if (!linkData && usbAvailable())
    {
        // the time spent waiting for the Sion is not a stalled picture
        linkData = true;
        diagShown = false;
        homeDrawn = false;
        lastAppliedAt = vblanks;
    }
    if (diagShown)
        showDiag();
    if (!mirroring)
    {
        // home: the Wi-Fi link and the screen, a step a frame
        static u32 lastHome = ~0u;
        if (lastHome != vblanks)
        {
            lastHome = vblanks;
            homeTick();
        }
    }
    else if (mirrorStats[1] != lastApplied)
    {
        lastApplied = mirrorStats[1];
        lastAppliedAt = vblanks;
    }
    else if (menuUp ? vblanks - lastDataAt > 120 :
             vblanks - lastAppliedAt > 120 && vblanks - lastDataAt > 60)
    {
        // nothing new for two seconds and no data for one (with the menu up, the Sion keeps
        // sending: no data for two): the link stopped. A big bundle can take seconds to come
        // in; asking for a full frame then would only throw it away and bring a bigger one
        // (run 7). Home meanwhile: the Sion sends a full bundle when it can again.
        if (!stallShown)
        {
            stallShown = true;
            stalls++;
            needFull = true;
            menuUp = false;
            leaveMirror = true;
        }
    }
    if (vblanks != lastScan && usbConfigured())
    {
        lastScan = vblanks;
        scanKeys();
        touchPosition t = {0};
        u32 keys = keysHeld();
        if (keys & KEY_TOUCH)
            touchRead(&t);
        u32 touch = t.px | (u32)t.py << 16;
        // (the menu has the screens: the full frame comes when it closes)
        u32 flags = (needFull && !stallManual && !menuUp ? MIRROR_INPUT_NEED_FULL : 0) | MIRROR_INPUT_MENUS;
        // a report when something changed, each new frame on screen (the Sion keeps the
        // game from running ahead of what the DS shows), and every 8 frames to show we're
        // still here: every card command is a chance for the DSpico to miss one
        u32 shown = shownFrame;
        if (keys != lastKeys || touch != lastTouch || flags != lastFlags || shown != lastShown ||
            vblanks - lastReport >= 8)
        {
            mirror_input in = {MIRROR_INPUT_MAGIC, keys, t.px, t.py, mirrorStats[1] | flags,
                               resyncs, allocFails, fullBundles, stalls,
                               usbDiag.deadReads, usbDiag.irqs, usbDiag.timedPolls,
                               usbDiag.recovered, usbDiag.rearmed, usbDiag.outDone, usbDiag.armTicks,
                               usbDiag.busyMax, (u16)usbDiag.timedHits, shown};
            if (usbSendReport(&in, sizeof in))
            {
                usbDiag.busyMax = 0;
                lastReport = vblanks;
                lastKeys = keys;
                lastTouch = touch;
                lastFlags = flags;
                lastShown = shown;
            }
        }
    }
}

static bool readBytes(void* dst, u32 n)
{
    if (!liveLink)
        return fread(dst, 1, n, recording) == n;
    u8* d = dst;
    while (n)
    {
        if (leaveMirror)
            return false;
        u32 got = usbRead(d, n);
        d += got;
        n -= got;
        if (n)
            idle();
    }
    return true;
}


// A live bundle damaged on the way must not be applied: one bad VRAM block
// number or record length writes somewhere it shouldn't. Checks every record.
static bool validBundle(const mirror_hdr* h)
{
    const u8* end = (const u8*)h + h->size;
    const mirror_rec* r = (const mirror_rec*)(h + 1);
    for (;;)
    {
        if ((const u8*)(r + 1) > end || (r->len & 3) || r->len > (u32)(end - (const u8*)(r + 1)))
            return false;
        const u32* p = (const u32*)(r + 1);
        u32 n = r->len >= 4 ? p[0] : 0;
        switch (r->type)
        {
        case MREC_END:
            return true;
        case MREC_VRAMCNT:
            if (r->len != 12) return false;
            break;
        case MREC_VRAM:
            if (r->len != 4 + n * (4 + MIRROR_BLOCK)) return false;
            for (u32 i = 0; i < n; i++)
            {
                u32 id = p[1 + i * (1 + MIRROR_BLOCK / 4)];
                u32 bank = id >> 16, blk = id & 0xFFFF;
                if (bank >= 9 || (blk + 1) * MIRROR_BLOCK > bankSize[bank]) return false;
            }
            break;
        case MREC_PAL:
        case MREC_OAM:
            if (r->len != 4 + n * (4 + MIRROR_ROW)) return false;
            for (u32 i = 0; i < n; i++)
            {
                u32 off = p[1 + i * (1 + MIRROR_ROW / 4)];
                if ((off & 3) || off + MIRROR_ROW > 0x800) return false;
            }
            break;
        case MREC_REGS:
        {
            if (r->len != 4 + n * sizeof(mirror_reg)) return false;
            const mirror_reg* reg = (const mirror_reg*)(p + 1);
            for (u32 i = 0; i < n; i++)
                if (!ioOk(reg[i].off, 2, true)) return false;
            break;
        }
        case MREC_LINES:
        {
            if (r->len != 4 + n * sizeof(mirror_line)) return false;
            const mirror_line* l = (const mirror_line*)(p + 1);
            for (u32 i = 0; i < n; i++)
                if (!ioOk(l[i].off, 2, true) || l[i].line >= 263) return false;
            break;
        }
        case MREC_GX:
        case MREC_GXDELTA:
            break;   // checked as the payload is built (decodeGX, pushGX)
        case MREC_MENU:
            if (r->len < sizeof(mirror_menu)) return false;
            break;   // its text is read within the record (uiMenu)
        case MREC_PAD:
            if (r->len < sizeof(mirror_pad)) return false;
            break;   // its text is read within the record (showPad)
        default:
            return false;
        }
        r = nextRec(r);
    }
}

static bool skipBytes(u32 n)
{
    static u8 scratch[512];
    while (n)
    {
        u32 k = n < sizeof scratch ? n : sizeof scratch;
        if (!readBytes(scratch, k))
            return false;
        n -= k;
    }
    return true;
}

static void lzTick(void)
{
    if (liveLink)
        usbPoll();
}

void uiPoll(void)
{
    lzTick();
}

// A packed bundle (MIRROR_FLAG_LZ) unpacked into a new buffer; NULL if there's no
// memory for it (*damaged false) or the data is damaged (*damaged true).
// Both loops run from ITCM as ARM code: course loads bring bundles of 300 KB, and
// inlined into Thumb code in main RAM they kept the DS off USB for a third of a second.
ARM_CODE ITCM_CODE __attribute__((noinline)) static int unpackLz(const u8* s, u32 sn, u8* d, u32 dn)
{
    return mirrorLzUnpack(s, sn, d, dn);
}

ARM_CODE ITCM_CODE __attribute__((noinline)) static u32 sumLz(const u32* w, u32 words)
{
    u32 sum = 0;
    while (words)
    {
        u32 n = words < 4096 ? words : 4096;
        sum = mirrorLzSumMore(sum, w, n);
        w += n;
        words -= n;
        lzTick();
    }
    return sum;
}

static mirror_hdr* unpackBundle(const mirror_hdr* p, bool* damaged)
{
    const u32* w = (const u32*)(p + 1);
    *damaged = true;
    if (p->size < sizeof *p + MIRROR_LZ_HEAD)
        return NULL;
    u32 rawSize = w[0], packed = w[2];
    if (rawSize < sizeof *p + sizeof(mirror_rec) || rawSize > 0x200000 || (rawSize & 3) ||
        packed > p->size - sizeof *p - MIRROR_LZ_HEAD)
        return NULL;
    mirror_hdr* b = malloc(rawSize);
    if (!b)
    {
        *damaged = false;
        return NULL;
    }
    *b = *p;
    b->size = rawSize;
    b->flags &= ~MIRROR_FLAG_LZ;
    u32 words = (rawSize - sizeof *b) / 4;
    if (!unpackLz((const u8*)(w + 3), packed, (u8*)(b + 1), rawSize - sizeof *b) ||
        mirrorLzSeal(sumLz((const u32*)(b + 1), words), p->frame, p->flags) != w[1])
    {
        free(b);
        return NULL;
    }
    return b;
}

// The game's menu is up on the TV (MREC_MENU): it takes the screens here too, the way it
// stands; when it closes, the game's screens come back with the full bundle that follows.
static mirror_hdr* volatile pending2D;
static volatile u32 holdFrames;

static void showMenu(const mirror_hdr* h)
{
    for (const mirror_rec* r = (const mirror_rec*)(h + 1); r->type != MREC_END; r = nextRec(r))
    {
        if (r->type != MREC_MENU)
            continue;
        const mirror_menu* m = (const mirror_menu*)(r + 1);
        if (!m->open)
        {
            // the full bundle is on its way: not a stalled picture meanwhile
            menuUp = false;
            lastAppliedAt = vblanks;
            return;
        }
        if (!menuUp)
        {
            // the game's last frame goes up first, then the menu takes its place
            while (pending2D || holdFrames)
                idle();
            lineList = NULL;
            lineCount = lineNext = 0;
            menuUp = true;
            stallShown = false;
            needFull = true;
        }
        uiMenu(m, r->len);
        homeDrawn = false;
    }
}

// The Sion's pad service has the DS (MREC_PAD), or lets go of it
static void showPad(const mirror_hdr* h)
{
    for (const mirror_rec* r = (const mirror_rec*)(h + 1); r->type != MREC_END; r = nextRec(r))
    {
        if (r->type != MREC_PAD)
            continue;
        const mirror_pad* p = (const mirror_pad*)(r + 1);
        const char* text = (const char*)(p + 1);
        const char* limit = (const char*)p + r->len;
        for (int i = 0; i < 2; i++)
        {
            const char* z = text < limit ? memchr(text, 0, limit - text) : NULL;
            u32 n = z ? (u32)(z - text) : 0;
            if (n >= sizeof padText[i])
                n = sizeof padText[i] - 1;
            memcpy(padText[i], text, n);
            padText[i][n] = 0;
            text = z ? z + 1 : limit;
        }
        if (!padText[0][0])
            strcpy(padText[0], "Controller for the Sion");
        padOn = p->on;
        padSeenAt = vblanks;
    }
}

// Live stream. Finds the next bundle header (resyncing byte by byte after a damaged one),
// and while a full refresh is wanted drops bundles that aren't full ones. Takes the menu
// and pad bundles itself. Gives up (NULL) only when the mirror ends: the picture stopped,
// or the Sion's pad service has the DS again.
static mirror_hdr* readBundleLive(void)
{
    for (;;)
    {
        mirror_hdr h;
        if (!readBytes(&h.magic, 4))
            return NULL;
        while (h.magic != MIRROR_MAGIC)
        {
            u8 b;
            if (!readBytes(&b, 1))
                return NULL;
            h.magic = (h.magic >> 8) | ((u32)b << 24);
        }
        if (!readBytes((u8*)&h + 4, sizeof h - 4))
            return NULL;
        if (h.size < sizeof h || h.size > 0x200000)
        {
            resyncs++;
            needFull = true;
            continue;
        }
        // The Sion packs every bundle, with a checksum of what it packed, and the packed
        // length must give the header's size. Otherwise this header was damaged, or the
        // magic number turned up inside the data: look for the next one rather than read
        // (up to 2 MB of) the stream as one bundle.
        u32 head[3];   // rawSize, rawSum, packedLen
        if (!(h.flags & MIRROR_FLAG_LZ) || h.size < sizeof h + sizeof head)
        {
            resyncs++;
            needFull = true;
            continue;
        }
        if (!readBytes(head, sizeof head))
            return NULL;
        if (head[2] > 0x200000 || h.size != mirrorLzSize(sizeof h, head[2]) || head[0] > 0x200000)
        {
            resyncs++;
            needFull = true;
            continue;
        }
        u32 body = h.size - sizeof h - sizeof head;
        if ((needFull && !(h.flags & (MIRROR_FLAG_FULL | MIRROR_FLAG_MENU | MIRROR_FLAG_PAD))) || stallManual)
        {
            if (!skipBytes(body))
                return NULL;
            continue;
        }
        mirror_hdr* b = malloc(h.size);
        if (!b)
        {
            allocFails++;
            needFull = true;
            if (!skipBytes(body))
                return NULL;
            continue;
        }
        *b = h;
        memcpy(b + 1, head, sizeof head);
        if (!readBytes((u8*)(b + 1) + sizeof head, body))
        {
            free(b);
            return NULL;
        }
        if (h.flags & MIRROR_FLAG_LZ)
        {
            bool damaged;
            mirror_hdr* raw = unpackBundle(b, &damaged);
            free(b);
            if (!raw)
            {
                if (damaged)
                    resyncs++;
                else
                    allocFails++;
                needFull = true;
                continue;
            }
            b = raw;
        }
        if (!validBundle(b))
        {
            free(b);
            resyncs++;
            needFull = true;
            continue;
        }
        if (h.flags & MIRROR_FLAG_PAD)
        {
            showPad(b);
            free(b);
            if (mirroring && padOn)
            {
                // the game ended on the Sion and its pad service took the DS back
                leaveMirror = true;
                return NULL;
            }
            continue;
        }
        if (!mirroring && !(h.flags & MIRROR_FLAG_FULL))
        {
            // a menu, or a bundle from before the mirror ended: nothing to show it over
            free(b);
            continue;
        }
        if (h.flags & MIRROR_FLAG_MENU)
        {
            showMenu(b);
            free(b);
            continue;
        }
        if (h.flags & MIRROR_FLAG_FULL)
        {
            needFull = false;
            fullBundles++;
            menuUp = false;
            if (!mirroring)
                startMirror();
        }
        return b;
    }
}

// a bundle's 3D commands, ready for pushGX; on the live link, a damaged delta
// means everything after it builds on what's missing: start over with a full bundle
static void readGX(const mirror_hdr* h)
{
    int r = decodeGX(h);
    if (r != GX_OK && liveLink)
    {
        if (r == GX_DAMAGED)
            resyncs++;
        else
            allocFails++;
        needFull = true;
    }
}

static mirror_hdr* readBundle(void)
{
    if (liveLink)
        return readBundleLive();
    mirror_hdr h;
    if (!readBytes(&h, sizeof h) || h.magic != MIRROR_MAGIC)
        return NULL;
    mirror_hdr* b = malloc(h.size);
    if (!b)
        return NULL;
    *b = h;
    if (!readBytes(b + 1, h.size - sizeof h))
    {
        free(b);
        return NULL;
    }
    return b;
}

static void clearHardware(void)
{
    for (int b = 0; b < 9; b++)
    {
        *vramcnt(b) = 0x80;
        for (u32 i = 0; i < bankSize[b]; i += 4)
            *(vu32*)(bankLCDC[b] + i) = 0;
        *vramcnt(b) = 0;
        curMap[b] = 0;
    }
    for (u32 i = 0; i < 2048; i += 4)
    {
        *(vu32*)(0x05000000 + i) = 0;
        *(vu32*)(0x07000000 + i) = 0;
    }
}

// Read back by the test harness: bundles applied, VBlanks that found the
// next bundle not ready yet, state (1 no NitroFS, 2 no file, 3 finished).
volatile u32 mirrorStats[4] __attribute__((used)) = {0x5354524D, 0, 0, 0}; // "MRTS"

// The bundle whose 3D writes are queued (pending2D, above); its 2D state goes in at VBlank
// start, from the IRQ, so it lands in the same VBlank as its SWAP_BUFFERS however long
// the main loop spends reading. holdFrames: 30 fps mode, VBlanks left that keep showing
// the current frame.

static void vblankHandler(void)
{
    mirror_hdr* h = pending2D;
    vblanks++;
    usbTick();
    flushLines();
    if (holdFrames)
    {
        holdFrames--;
        lineNext = 0;
        return;
    }
    if (!h)
    {
        mirrorStats[2]++;
        lineNext = 0;   // same frame again: replay its line changes again
        return;
    }
    apply2D(h);
    if (h->flags & MIRROR_FLAG_HOLD)
        holdFrames = 1;
    mirrorStats[1]++;
    shownFrame = h->frame;
    pending2D = NULL;
}

static void openSource(void)
{
    // a recording inside this .nds (NitroFS) or next to it on the SD card,
    // else live from the Sion
    if (nitroFSInit(NULL))
        recording = fopen("nitro:/mirror.bin", "rb");
    if (!recording && fatInitDefault())
    {
        recording = fopen("fat:/mirror.bin", "rb");
        if (!recording)
            recording = fopen("sd:/mirror.bin", "rb");
    }
    if (recording)
        return;
    liveLink = true;
    mirrorStats[3] = 4;
    usbStart();
    goHome();
}

int main(int argc, char** argv)
{
    // Download Play stages a program in the heap and moves it into place from there; the
    // top of main RAM (the header a booted program reads) must stay out of it
    u8* heapTop = getHeapLimit();
    if (heapTop > (u8*)SLBOOT_STAGE_END)
        reduceHeapSize(heapTop - (u8*)SLBOOT_STAGE_END);
    powerOn(POWER_ALL);
    clearHardware();
    irqSet(IRQ_VBLANK, vblankHandler);
    irqSet(IRQ_HBLANK, hblankHandler);

    openSource();
    for (;;)
    {
        mirror_hdr* cur = readBundle();
        if (cur)
            readGX(cur);
        mirror_hdr* old = NULL;
        while (cur)
        {
            // start early in the visible lines so the 3D engine reaches SWAP_BUFFERS before VBlank
            if (liveLink && (cur->flags & MIRROR_FLAG_FULL) && (fullBundles > 1 || uiShown()))
            {
                // a full refresh: it assumes cleared hardware, like the first bundle
                swiWaitForVBlank();
                lineList = NULL;
                lineCount = lineNext = 0;
                uiLeave();
                clearHardware();
                stallShown = false;
            }
            else if (stallShown && liveLink && (cur->flags & MIRROR_FLAG_FULL))
                stallShown = false;
            while (REG_VCOUNT >= 96)
                idle();
            pushGX();
            pending2D = cur;
            mirror_hdr* next = readBundle();
            if (next)
                readGX(next);
            while (pending2D || holdFrames)
                idle();
            free(old);
            old = cur;   // still on screen (its line list) until the next one is applied
            cur = next;
        }
        if (liveLink)
        {
            // the mirror ended: its memory and screens go, home comes back
            swiWaitForVBlank();
            irqDisable(IRQ_HBLANK);
            lineList = NULL;
            lineCount = lineNext = 0;
            free(old);
            free(gxPayload);
            gxPayload = NULL;
            gxWords = 0;
            if (mirroring)
                goHome();
            continue;
        }
        mirrorStats[3] = 3;
        // play it again from the start: the first bundle assumes cleared hardware
        swiWaitForVBlank();
        irqDisable(IRQ_HBLANK);
        lineList = NULL;
        lineCount = lineNext = 0;
        free(old);
        free(gxPayload);
        gxPayload = NULL;
        gxWords = 0;
        clearHardware();
        rewind(recording);
    }
}
