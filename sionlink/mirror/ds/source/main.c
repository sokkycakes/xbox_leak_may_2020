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

// The 3D commands of the last bundle read, as an MREC_GX payload. A delta
// record copies from the one before.
static u32* gxPayload;
static u32 gxWords;

static void decodeGX(const mirror_hdr* h)
{
    u32* prev = gxPayload;
    u32 prevWords = gxWords;
    gxPayload = NULL;
    gxWords = 0;
    for (const mirror_rec* r = (const mirror_rec*)(h + 1); r->type != MREC_END; r = nextRec(r))
    {
        const u32* p = (const u32*)(r + 1);
        if (r->type == MREC_GX)
        {
            gxWords = r->len / 4;
            gxPayload = malloc(r->len);
            if (gxPayload)
                memcpy(gxPayload, p, r->len);
        }
        else if (r->type == MREC_GXDELTA)
        {
            u32 n = *p++, i = 0;
            gxPayload = malloc(n * 4);
            if (!gxPayload)
                break;
            gxWords = n;
            const u32* recEnd = (const u32*)((const u8*)p - 4 + r->len);
            while (i < n)
            {
                if (p >= recEnd)
                    break;
                u32 op = *p++;
                u32 len = op & 0x7FFFFFFF;
                if (len > n - i || (!(op & 0x80000000) && p + len > recEnd))
                {
                    // damaged: no 3D this frame
                    free(gxPayload);
                    gxPayload = NULL;
                    gxWords = 0;
                    break;
                }
                if (op & 0x80000000)
                {
                    u32 src = *p++;
                    if (prev && src + len <= prevWords)
                        memcpy(gxPayload + i, prev + src, len * 4);
                }
                else
                {
                    memcpy(gxPayload + i, p, len * 4);
                    p += len;
                }
                i += len;
            }
        }
    }
    free(prev);
}

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
        if (p + n > end || run->off >= 0x1070)
            break;
        if (run->off == 0x304)
        {
            // power control, in order with the 3D writes; screen swap stays as the 2D state set it
            for (u32 k = 0; k < n; k++)
                REG_POWERCNT = (*p++ & 0x7FFF) | (REG_POWERCNT & 0x8000);
        }
        else if (code == 0)
            for (u32 k = 0; k < n; k++) *(vu32*)addr = *p++;
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
static bool liveLink;
static u32 vblanks, lastReport;

static void clearHardware(void);
static bool linkData;     // the Sion has started sending
// Live: the screens no longer match the stream (a bundle was lost, or the
// stall screen took over); bundles are dropped until the Sion sends a full one
static bool needFull = true;
static bool stallShown, stallManual;
static u32 comboSince;   // VBlank count when L+R+SELECT went down, 0 while it isn't held
static bool comboDone;
static u32 lastApplied, lastAppliedAt;
static u32 resyncs, allocFails, fullBundles;
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
    if (stallManual)
        printf("\x1b[0;0HSion Link: status            \n\n");
    else
        printf("\x1b[0;0HSion Link: picture stopped %4lu\n\n", (vblanks - lastAppliedAt) / 60);
    printf("frames shown  %lu\n", mirrorStats[1]);
    printf("full refresh  %lu\n", fullBundles);
    printf("resyncs       %lu\n", resyncs);
    printf("out of memory %lu\n", allocFails);
    printf("data waiting  %lu\n", usbAvailable());
    printf("usb events    %lu\n", usbDiag.events);
    printf("usb state     %-12s\n", usbDiag.deadReads ? "DSpico silent" : usbConfigured() ? "connected" : "lost");
    printf("silent reads  %lu\n", usbDiag.deadReads);
    printf("nudges %lu, recovered %lu\n", usbDiag.nudges, usbDiag.recovered);
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
    else if (vblanks - lastAppliedAt > 120)
    {
        // nothing new for two seconds: show what the link is doing
        if (!stallShown)
        {
            stallShown = true;
            needFull = true;
            showStallScreen();
        }
        showStall();
    }
    if (vblanks != lastReport && usbConfigured())
    {
        scanKeys();
        touchPosition t = {0};
        u32 keys = keysHeld();
        if (keys & KEY_TOUCH)
            touchRead(&t);
        mirror_input in = {MIRROR_INPUT_MAGIC, keys, t.px, t.py,
                           mirrorStats[1] | (needFull && !stallManual ? MIRROR_INPUT_NEED_FULL : 0)};
        if (usbSendReport(&in, sizeof in))
            lastReport = vblanks;
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

static bool regOk(u32 off)
{
    return !(off & 1) && off + 2 <= 0x1070;
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
                if (!regOk(reg[i].off)) return false;
            break;
        }
        case MREC_LINES:
        {
            if (r->len != 4 + n * sizeof(mirror_line)) return false;
            const mirror_line* l = (const mirror_line*)(p + 1);
            for (u32 i = 0; i < n; i++)
                if (!regOk(l[i].off) || l[i].line >= 263) return false;
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
        u32 body = h.size - sizeof h;
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
        readBytes(b + 1, body);
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
            decodeGX(cur);
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
                decodeGX(next);
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
