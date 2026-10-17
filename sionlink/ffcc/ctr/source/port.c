// FFCC GBA client on the 3DS: the GBA hardware the client expects (see
// ctr_port.h), its JOY port fed over Sion Link (ffproto.h), and start-up.
//
// Same shape as the DS port (ds/source/port.c): the client's main loop calls
// VBlankIntrWait() once a frame, and that is where the port does what a GBA
// would have done behind its back: the vertical blank interrupt, the picture
// (drawn by gbappu.c and shown 1.5x, filling the top screen's height), the
// buttons, and the JoyBus traffic from the Sion, applied one GameCube
// command at a time with the client's serial interrupt run after each.

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "ffproto.h"
#include "gbappu.h"
#include "sllink.h"

// The GBA screen at 1.5x, 360x240 in the middle of the 400x240 top screen
// (the full width would stretch it)
#define TOP_W (GBA_W * 3 / 2)
#define OFS_X ((400 - TOP_W) / 2)

// The client (built from the decompilation)
void AgbMain(void);
void VBlankIntr(void);
void Link_JoyIntr(void);
unsigned char Link_IsConnected(void);

// The GBA's memory (ctr_port.h)
u8 gPortIo[0x400] __attribute__((aligned(4)));
u16 gPortPltt[0x200];
u8 gPortVram[0x18000] __attribute__((aligned(4)));
u16 gPortOam[0x200];
vu16 gPortIE, gPortIME, gPortDummy16;
void *gPortDummyPtr;
u8 gPortMbHeader[0x100], gPortCartHeader[0x100];
u8 gPortDownload[0xC000] __attribute__((aligned(32)));

#define GBA_IO16(o) (*(vu16 *)(gPortIo + (o)))

static PrintConsole bottom;
static jmp_buf restart;

// Display
// =======

static u16 frame[GBA_W * GBA_H];

// GBA BGR555 to the top screen's RGB565 (green's low bit from its top bit)
static inline u16 To565(u16 c)
{
    return ((c & 31) << 11) | ((c & 0x3E0) << 1) | ((c >> 4) & 0x20) | ((c >> 10) & 31);
}

// The average of two RGB565 colors
static inline u16 Mix(u16 a, u16 b)
{
    return (((a ^ b) & 0xF7DE) >> 1) + (a & b);
}

// Time spent drawing the picture and running the game, per frame (in
// ARM11 ticks, averaged on the bottom screen)
static u64 ticks_ppu, ticks_draw, ticks_game, ticks_left;

static void Present(void)
{
    u64 t0 = svcGetSystemTick();
    gba_ppu ppu = { gPortIo, gPortPltt, gPortVram, gPortOam };
    GbaPpuDraw(&ppu, frame);
    ticks_draw += svcGetSystemTick() - t0;

    // Each 2x2 block of GBA pixels becomes 3x3, the middle row and column
    // the average of their neighbors. The top framebuffer is stored a
    // column at a time, bottom to top
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int x = 0; x < GBA_W; x += 2)
    {
        u16 cols[3][GBA_H];
        for (int y = 0; y < GBA_H; y++)
        {
            cols[0][y] = To565(frame[y * GBA_W + x]);
            cols[2][y] = To565(frame[y * GBA_W + x + 1]);
            cols[1][y] = Mix(cols[0][y], cols[2][y]);
        }
        for (int i = 0; i < 3; i++)
        {
            u16 *out = fb + (OFS_X + x / 2 * 3 + i) * 240 + 239;
            const u16 *c = cols[i];
            for (int y = 0; y < GBA_H; y += 2)
            {
                *out-- = c[y];
                *out-- = Mix(c[y], c[y + 1]);
                *out-- = c[y + 1];
            }
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    ticks_ppu += svcGetSystemTick() - t0;
#ifdef PPU_DUMP
    // The display state every 10 s, for checking the picture on a PC
    static int frames;
    if (++frames % 600 == 0)
    {
        char path[32];
        snprintf(path, sizeof(path), "sdmc:/ffppu%d.bin", frames / 600);
        FILE *f = fopen(path, "wb");
        if (f)
        {
            fwrite(gPortIo, 1, 0x400, f);
            fwrite(gPortPltt, 1, 0x400, f);
            fwrite(gPortVram, 1, 0x18000, f);
            fwrite(gPortOam, 1, 0x400, f);
            fclose(f);
        }
    }
#endif
}

static void ClearTop(void)
{
    for (int i = 0; i < 2; i++)
    {
        memset(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), 0, 400 * 240 * 2);
        gfxSwapBuffers();
    }
}

// Buttons: the GBA's ten, active low; the circle pad works as the D-pad
static void ReadKeys(void)
{
    static const u32 map[10] = { KEY_A, KEY_B, KEY_SELECT, KEY_START, KEY_RIGHT,
                                 KEY_LEFT, KEY_UP, KEY_DOWN, KEY_R, KEY_L };
    u32 held = hidKeysHeld();
    u16 v = 0;
    for (int i = 0; i < 10; i++)
        if (held & map[i])
            v |= 1 << i;
    GBA_IO16(0x130) = ~v & 0x3FF;
}

// DMA and BIOS
// ============

// GBA DMA, done at once whatever its start timing
void PortDma(u32 src, u32 dst, u32 control)
{
    if (!(control & 0x80000000))
        return;
    u32 count = control & 0xFFFF;
    bool words = control & 0x04000000;
    int src_step = ((control >> 23) & 3) == 2 ? 0 : 1;
    if (!count)
        count = 0x10000;
    if (words)
    {
        vu32 *s = (vu32 *)src, *d = (vu32 *)dst;
        for (u32 i = 0; i < count; i++, s += src_step)
            d[i] = *s;
    }
    else
    {
        vu16 *s = (vu16 *)src, *d = (vu16 *)dst;
        for (u32 i = 0; i < count; i++, s += src_step)
            d[i] = *s;
    }
}

void CpuFastSet(const void *src, void *dst, u32 control)
{
    u32 count = ((control & 0x1FFFFF) + 7) & ~7u; // the BIOS works in blocks of 8 words
    vu32 *d = dst;
    const vu32 *s = src;
    if (control & 0x01000000)
    {
        u32 v = *s;
        for (u32 i = 0; i < count; i++)
            d[i] = v;
    }
    else
    {
        for (u32 i = 0; i < count; i++)
            d[i] = s[i];
    }
}

// The BIOS's LZ77 format: a 4-byte header (type 0x10, 24-bit size), then
// flag bytes each covering eight literals or back references
static void LZ77(const u8 *src, u8 *dst)
{
    u32 size = (src[1] | (src[2] << 8) | (src[3] << 16));
    src += 4;
    u32 out = 0;
    while (out < size)
    {
        u8 flags = *src++;
        for (int i = 0; i < 8 && out < size; i++, flags <<= 1)
        {
            if (!(flags & 0x80))
            {
                dst[out++] = *src++;
                continue;
            }
            int len = (src[0] >> 4) + 3;
            int disp = (((src[0] & 15) << 8) | src[1]) + 1;
            src += 2;
            for (int j = 0; j < len && out < size; j++, out++)
                dst[out] = dst[out - disp];
        }
    }
}

void LZ77UnCompWram(const void *src, void *dst)
{
    LZ77(src, dst);
}

void LZ77UnCompVram(const void *src, void *dst)
{
    LZ77(src, dst);
}

// Sound isn't ported yet
void m4aSoundInit(void) {}
void m4aSoundMain(void) {}
void m4aSoundVSync(void) {}
void m4aSongNumStart(u16 n) {}
void m4aMPlayAllStop(void) {}
void intr_main(void) {}

// The GameCube reset the GBA again and again: start the client over
void JoyBus_HardReset(void)
{
    longjmp(restart, 1);
}

// The JOY port
// ============
//
// As on the DS (ds/source/port.c): the GameCube's reads and writes change
// JOYSTAT and JOYCNT and raise the serial interrupt. The client reads
// JOY_RECV (which empties it), writes JOY_TRANS (which fills it), and keeps
// two flags of its own in JOYSTAT bits 4-5.

static vu32 joy_recv, joy_trans;
static vu16 joy_stat, joy_cnt;
static bool recv_full, trans_full;

vu32 *PortJoyRecv(void)
{
    recv_full = false;
    return &joy_recv;
}

vu32 *PortJoyTrans(void)
{
    trans_full = true;
    return &joy_trans;
}

static u16 JoyStat(void)
{
    return (joy_stat & 0x30) | (recv_full ? 2 : 0) | (trans_full ? 8 : 0);
}

vu16 *PortJoyStat(void)
{
    joy_stat = JoyStat();
    return &joy_stat;
}

vu16 *PortJoyCnt(void)
{
    return &joy_cnt;
}

static void JoyIrq(u16 cause)
{
    joy_cnt = (joy_cnt & ~7) | cause;
    if ((gPortIE & 0x80) && gPortIME)
        Link_JoyIntr();
    joy_cnt &= ~7;
}

static u32 joy_batches, joy_resent;

static void JoyBatch(const u8 *body, int len)
{
    static u16 last_seq;
    static bool have_last;
    static u8 reply[sizeof(ff_joy) + FF_MAX_OPS * sizeof(ff_result)];
    static int reply_len;

    ff_joy j;
    memcpy(&j, body, sizeof(j));
    if (j.count > FF_MAX_OPS || len < (int)(sizeof(ff_joy) + j.count * sizeof(ff_op)))
        return;
    if (have_last && j.seq == last_seq)
    {
        SlSend(SL_GAME, reply, reply_len); // our answer got lost
        joy_resent++;
        return;
    }

    ff_joy *r = (ff_joy *)reply;
    r->kind = FF_REPLY;
    r->count = j.count;
    r->seq = j.seq;
    ff_result *res = (ff_result *)(reply + sizeof(ff_joy));
    const ff_op *ops = (const ff_op *)(body + sizeof(ff_joy));
    for (int i = 0; i < j.count; i++)
    {
        ff_op op;
        memcpy(&op, &ops[i], sizeof(op));
        ff_result out = { .cmd = op.cmd };
        switch (op.cmd)
        {
        case FF_CMD_RESET:
            out.stat = JoyStat();
            JoyIrq(1);
            break;
        case FF_CMD_STATUS:
            out.stat = JoyStat();
            break;
        case FF_CMD_READ:
            out.data = joy_trans;
            out.stat = JoyStat();
            trans_full = false;
            JoyIrq(4);
            break;
        case FF_CMD_WRITE:
            out.stat = JoyStat();
            joy_recv = op.data;
            recv_full = true;
            JoyIrq(2);
            break;
        }
        memcpy(&res[i], &out, sizeof(out));
    }
    joy_batches++;
    reply_len = sizeof(ff_joy) + j.count * sizeof(ff_result);
    last_seq = j.seq;
    have_last = true;
    SlSend(SL_GAME, reply, reply_len);
}

static void BootParams(const u8 *body, int len)
{
    if (len < (int)sizeof(ff_boot))
        return;
    ff_boot b;
    memcpy(&b, body, sizeof(b));
    memcpy(gPortMbHeader + 0xAC, &b.idle_word, 4);
    gPortMbHeader[0xC4] = b.boot_mode ? b.boot_mode : 1;
    gPortMbHeader[0xC5] = b.player_no;
    memcpy(gPortMbHeader + 0xC8, &b.host_id, 4);
}

// Once a frame
// ============

static int player;
static bool joined;

static void Status(void)
{
    consoleSelect(&bottom);
    printf("\x1b[1;1H  Crystal Chronicles GBA link\n");
    printf("         over Sion Link\n\n");
    if (!joined)
        printf(" %-38s", "Looking for the Sion...");
    else if (!player)
        printf(" %-38s", "Linked. Saying hello...");
    else
        printf(" Player %d on the Sion%-18s", player, "");
    printf("\x1b[5;1HGameCube link: %-12s\n", Link_IsConnected() ? "connected" : "waiting");
    printf("\x1b[6;1HJoyBus: %lu batches   \n", (unsigned long)joy_batches);
    printf("\x1b[7;1Hresent answers: %lu   \n", (unsigned long)joy_resent);
    double ms = 1000.0 / SYSCLOCK_ARM11 / 30;
    printf("\x1b[9;1Hpicture %4.1f ms (drawing %4.1f)   \n", ticks_ppu * ms, ticks_draw * ms);
    printf("\x1b[10;1Hgame %4.1f ms   \n", ticks_game * ms);
    ticks_ppu = ticks_draw = ticks_game = 0;
    printf("\x1b[28;1HHOME or L+R+START+SELECT: HOME Menu");
}

static void Say(const char *s);

static void Pump(void)
{
    u32 now = SlMillis();
    if (!joined)
        return;
    // Closing the lid sleeps the 3DS, which leaves the network: link again
    // on waking (or after 8 s without the Sion, which says welcome every 3 s)
    if (SlRejoin(Say, 8000))
        consoleClear();
    player = SlKeepAlive(FF_APP_ID, now);
    const sl_hdr *h;
    const u8 *body;
    while (SlNext(&h, &body))
    {
        if (h->type != SL_GAME || h->len < 1)
            continue;
        if (body[0] == FF_JOY && h->len >= sizeof(ff_joy))
            JoyBatch(body, h->len);
        else if (body[0] == FF_BOOT)
            BootParams(body, h->len);
    }
}

static void Quit(void)
{
    SlExit();
    gfxExit();
    exit(0);
}

void VBlankIntrWait(void)
{
    if (ticks_left)
        ticks_game += svcGetSystemTick() - ticks_left;
    if (!aptMainLoop())
        Quit();
    Present();
    gspWaitForVBlank();
    hidScanInput();
    // In case HOME doesn't get through: the same trip to the HOME Menu
    const u32 home = KEY_L | KEY_R | KEY_START | KEY_SELECT;
    if ((hidKeysHeld() & home) == home && (hidKeysDown() & home))
        aptJumpToHomeMenu();
    ReadKeys();
    if ((gPortIE & 1) && gPortIME)
        VBlankIntr();
    Pump();
    static int n;
    if (++n % 30 == 0)
        Status();
    ticks_left = svcGetSystemTick();
}

// Start
// =====

static void Say(const char *s)
{
    consoleSelect(&bottom);
    consoleClear();
    printf("\x1b[1;1H  Crystal Chronicles GBA link\n");
    printf("         over Sion Link\n\n%s\n", s);
}

int main(int argc, char *argv[])
{
    osSetSpeedupEnable(true); // 804 MHz and the L2 cache on a New 3DS
    gfxInit(GSP_RGB565_OES, GSP_BGR8_OES, false);
    consoleInit(GFX_BOTTOM, &bottom);
    ClearTop();
    GBA_IO16(0x130) = 0x3FF;
    memcpy(gPortCartHeader + 0xAC, "RELS", 4);

    Say("Starting wireless...");
    if (SlInit() && SlJoin(Say))
        joined = true;
    else if (!aptMainLoop())
        Quit();

    // What a GameCube writes into the multiboot image before booting it;
    // Link_Init() reads it once, at start
    Say("Waiting for the FFCC host on the Sion...");
    for (u32 t0 = SlMillis(); joined && !gPortMbHeader[0xC4] && SlMillis() - t0 < 5000;)
    {
        if (!aptMainLoop())
            Quit();
        gspWaitForVBlank();
        Pump();
    }
    consoleClear();
    setjmp(restart);
    AgbMain();
    Quit();
    return 0;
}
