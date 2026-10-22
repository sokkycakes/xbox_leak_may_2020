// Mirror replay client: plays nitro:/mirror.bin, a recording of what an
// emulated game did to its display hardware, on this DS's own hardware.
//
// Per frame: during the visible lines push the bundle's 3D writes (their
// SWAP_BUFFERS lands at the next VBlank, as it did in the emulator), then in
// VBlank apply its 2D state. Both show up together in the next frame.

#include <nds.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mirror_format.h"

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
            while (i < n)
            {
                u32 op = *p++;
                u32 len = op & 0x7FFFFFFF;
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
    if (!gxPayload)
        return;
    const u32* p = gxPayload;
    u32 runs = *p++;
    for (u32 i = 0; i < runs; i++)
    {
        const mirror_run* run = (const mirror_run*)p++;
        u32 addr = 0x04000000 + run->off;
        u32 n = run->info & 0x3FFF, code = run->info >> 14;
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

static mirror_hdr* readBundle(FILE* f)
{
    mirror_hdr h;
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != MIRROR_MAGIC)
        return NULL;
    mirror_hdr* b = malloc(h.size);
    if (!b)
        return NULL;
    *b = h;
    if (fread(b + 1, 1, h.size - sizeof h, f) != h.size - sizeof h)
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

int main(int argc, char** argv)
{
    powerOn(POWER_ALL);
    clearHardware();
    irqSet(IRQ_VBLANK, vblankHandler);
    irqSet(IRQ_HBLANK, hblankHandler);

    if (!nitroFSInit(NULL))
    {
        mirrorStats[3] = 1;
        for (;;) swiWaitForVBlank();
    }
    FILE* f = fopen("nitro:/mirror.bin", "rb");
    if (!f)
    {
        mirrorStats[3] = 2;
        for (;;) swiWaitForVBlank();
    }

    mirror_hdr* cur = readBundle(f);
    if (cur)
        decodeGX(cur);
    mirror_hdr* old = NULL;
    while (cur)
    {
        // start early in the visible lines so the 3D engine reaches SWAP_BUFFERS before VBlank
        while (REG_VCOUNT >= 96)
            ;
        pushGX();
        pending2D = cur;
        mirror_hdr* next = readBundle(f);
        if (next)
            decodeGX(next);
        while (pending2D || holdFrames)
            ;
        free(old);
        old = cur;   // still on screen (its line list) until the next one is applied
        cur = next;
    }
    mirrorStats[3] = 3;
    for (;;)
        swiWaitForVBlank();
}
