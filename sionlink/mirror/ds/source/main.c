// Mirror replay client: plays nitro:/mirror.bin, a recording of what an
// emulated game did to its display hardware, on this DS's own hardware.
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
#include "usblink.h"

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
// Live: the screens no longer match the stream (a bundle was lost, or the
// stall screen took over); bundles are dropped until the Sion sends a full one
static bool needFull = true;
static bool stallShown, stallManual;
static u32 comboSince;   // VBlank count when L+R+SELECT went down, 0 while it isn't held
static bool comboDone;
static u32 lastApplied, lastAppliedAt;
static u32 lastOutBytes, lastDataAt;   // when data last arrived from the Sion
static u32 resyncs, allocFails, fullBundles, stalls;
static void showStall(void);
static void showStallScreen(void);
static u32 lastDiag = ~0u;

// Before the Sion sends anything, the bottom screen shows what the link has seen
static void showDiag(void)
{
    if (vblanks - lastDiag < 15)
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

// The picture stopped: what the link and the stream are doing
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
    // L+R+SELECT held for a second: show or hide the status page
    if (linkData && (keysHeld() & (KEY_L | KEY_R | KEY_SELECT)) == (KEY_L | KEY_R | KEY_SELECT))
    {
        if (!comboSince)
            comboSince = vblanks | 1;
        if (!comboDone && vblanks - comboSince >= 60)
        {
            comboDone = true;
            stallManual = !stallManual;
            if (stallManual)
            {
                // the game's frames would draw over the page: drop them until it closes
                stallShown = true;
                needFull = true;
                showStallScreen();
            }
            else
            {
                // back to the game: a full frame repaints everything
                stallShown = false;
                needFull = true;
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
    if (!linkData)
    {
        if (usbAvailable())
        {
            // the first bundle sets every display register; start from cleared VRAM.
            // The time spent waiting for the Sion is not a stalled picture.
            linkData = true;
            lastAppliedAt = vblanks;
            clearHardware();
        }
        else
            showDiag();
    }
    else if (mirrorStats[1] != lastApplied)
    {
        lastApplied = mirrorStats[1];
        lastAppliedAt = vblanks;
    }
    else if (vblanks - lastAppliedAt > 120 && vblanks - lastDataAt > 60)
    {
        // nothing new for two seconds and no data for one: show what the link is doing.
        // A big bundle can take seconds to come in; asking for a full frame then
        // would only throw it away and bring a bigger one (run 7).
        if (!stallShown)
        {
            stallShown = true;
            stalls++;
            needFull = true;
            showStallScreen();
        }
        showStall();
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
        u32 flags = needFull && !stallManual ? MIRROR_INPUT_NEED_FULL : 0;
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
                               usbDiag.busyMax, 0, shown};
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
        default:
            return false;
        }
        r = nextRec(r);
    }
}

static void skipBytes(u32 n)
{
    static u8 scratch[512];
    while (n)
    {
        u32 k = n < sizeof scratch ? n : sizeof scratch;
        readBytes(scratch, k);
        n -= k;
    }
}

static void lzTick(void)
{
    if (liveLink)
        usbPoll();
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

// Live stream: never gives up. Finds the next bundle header (resyncing byte by
// byte after a damaged one), and while a full refresh is wanted drops bundles
// that aren't full ones.
static mirror_hdr* readBundleLive(void)
{
    for (;;)
    {
        mirror_hdr h;
        readBytes(&h.magic, 4);
        while (h.magic != MIRROR_MAGIC)
        {
            u8 b;
            readBytes(&b, 1);
            h.magic = (h.magic >> 8) | ((u32)b << 24);
        }
        readBytes((u8*)&h + 4, sizeof h - 4);
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
        readBytes(head, sizeof head);
        if (head[2] > 0x200000 || h.size != mirrorLzSize(sizeof h, head[2]) || head[0] > 0x200000)
        {
            resyncs++;
            needFull = true;
            continue;
        }
        u32 body = h.size - sizeof h - sizeof head;
        if ((needFull && !(h.flags & MIRROR_FLAG_FULL)) || stallManual)
        {
            skipBytes(body);
            continue;
        }
        mirror_hdr* b = malloc(h.size);
        if (!b)
        {
            allocFails++;
            needFull = true;
            skipBytes(body);
            continue;
        }
        *b = h;
        memcpy(b + 1, head, sizeof head);
        readBytes((u8*)(b + 1) + sizeof head, body);
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
        if (h.flags & MIRROR_FLAG_FULL)
        {
            needFull = false;
            fullBundles++;
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

// The bundle whose 3D writes are queued; its 2D state goes in at VBlank start,
// from the IRQ, so it lands in the same VBlank as its SWAP_BUFFERS however long
// the main loop spends reading.
static mirror_hdr* volatile pending2D;
static volatile u32 holdFrames;  // 30 fps mode: VBlanks left that keep showing the current frame

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

// Plain colour on both screens: blue while waiting for the Sion
static void showColor(u16 color)
{
    REG_DISPCNT = MODE_0_2D;
    REG_DISPCNT_SUB = MODE_0_2D;
    BG_PALETTE[0] = color;
    BG_PALETTE_SUB[0] = color;
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
    showColor(RGB15(0, 8, 24));
    consoleDemoInit();
    usbStart();
}

int main(int argc, char** argv)
{
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
            if (liveLink && (cur->flags & MIRROR_FLAG_FULL) && fullBundles > 1)
            {
                // a full refresh: it assumes cleared hardware, like the first bundle
                swiWaitForVBlank();
                lineList = NULL;
                lineCount = lineNext = 0;
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
