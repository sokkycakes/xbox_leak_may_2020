// FFCC GBA client on the DS: the GBA hardware the client expects (see
// ds_port.h), its JOY port fed over Sion Link (ffproto.h), and start-up.
//
// The client's main loop calls VBlankIntrWait() once a frame. That is where
// the port does everything a GBA would have done behind its back: the
// vertical blank interrupt, the display registers, and the JoyBus traffic
// from the Sion, applied one GameCube command at a time with the client's
// serial interrupt run after each, as on the real thing.

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "ffproto.h"
#include "sllink.h"

// The GBA screen, centered on the DS's top screen
#define OFS_X 8
#define OFS_Y 16

// The client (built from the decompilation)
void AgbMain(void);
void VBlankIntr(void);
void Link_JoyIntr(void);
u8 Link_IsConnected(void);

// Shadows and stand-ins (ds_port.h)
vu16 gPortDispcnt, gPortIE, gPortIME, gPortDummy16;
void *gPortDummyPtr;
vu16 gPortScroll[8];
u8 gPortMbHeader[0x100], gPortCartHeader[0x100];

static PrintConsole bottom;
static jmp_buf restart;

// Display
// =======

static void ApplyDisplay(void)
{
    // GBA DISPCNT: bit 5 OBJ during HBlank and bit 6 1D OBJ tiles sit
    // elsewhere on the DS; bits 0-2 mode, 7 blank, 8-15 layers and windows
    // match. WIN0 and WIN1 frame the 240x160 picture (see Init()).
    u32 g = gPortDispcnt;
    u32 d = (g & 0x0007) | (g & 0xFF80) | BIT(16) | DISPLAY_WIN0_ON | DISPLAY_WIN1_ON;
    if (g & 0x40)
        d |= BIT(4);
    if (g & 0x20)
        d |= BIT(23);
    REG_DISPCNT = d;

    vu16 *scroll = (vu16 *)0x04000010;
    for (int i = 0; i < 4; i++)
    {
        scroll[i * 2] = gPortScroll[i * 2] - OFS_X;
        scroll[i * 2 + 1] = gPortScroll[i * 2 + 1] - OFS_Y;
    }
}

// Sprites move with the picture
static void ShiftOam(u16 *oam, int count)
{
    for (int i = 0; i < count; i++, oam += 4)
    {
        oam[0] = (oam[0] & 0xFF00) | ((oam[0] + OFS_Y) & 0x00FF);
        oam[1] = (oam[1] & 0xFE00) | ((oam[1] + OFS_X) & 0x01FF);
    }
}

// DMA and BIOS
// ============

// GBA DMA, done by the CPU: the DS's DMA wouldn't see what the ARM9 has in
// its data cache.
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
    u32 bytes = count * (words ? 4 : 2);
    if (dst >= 0x07000000 && dst < 0x07000400)
        ShiftOam((u16 *)dst, bytes / 8);
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

void LZ77UnCompWram(const void *src, void *dst)
{
    decompress(src, dst, LZ77);
}

void LZ77UnCompVram(const void *src, void *dst)
{
    decompress(src, dst, LZ77Vram);
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
// On a GBA, the GameCube's reads and writes change JOYSTAT and JOYCNT and
// raise the serial interrupt. The client reads JOY_RECV (which empties it),
// writes JOY_TRANS (which fills it), and keeps two flags of its own in
// JOYSTAT bits 4-5. The register macros call these functions, so the side
// effects happen as each register is touched.

static vu32 joy_recv, joy_trans;
static vu16 joy_stat, joy_cnt;
static bool recv_full, trans_full;

vu32 *PortJoyRecv(void)
{
    recv_full = false; // the client is reading it
    return &joy_recv;
}

vu32 *PortJoyTrans(void)
{
    trans_full = true; // the client only ever writes it
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

static u32 joy_ops, joy_batches, joy_resent;

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
    joy_ops += j.count;
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
    printf("\x1b[0;0H  Crystal Chronicles GBA link\n");
    printf("       over Sion Link\n\n");
    if (!joined)
        printf("%-32s", "Looking for the Sion...");
    else if (!player)
        printf("%-32s", "Linked. Saying hello...");
    else
        printf("Player %d on the Sion%-12s", player, "");
    printf("\x1b[4;0HGameCube link: %-12s\n", Link_IsConnected() ? "connected" : "waiting");
    printf("\x1b[5;0H%-32s", "");
    printf("\x1b[5;0HJoyBus: %lu batches, %lu cmds\n", (unsigned long)joy_batches,
           (unsigned long)joy_ops);
    printf("\x1b[6;0Hresent answers: %lu   \n", (unsigned long)joy_resent);
}

static void Pump(void)
{
    u32 now = SlMillis();
    if (!joined)
        return;
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

void VBlankIntrWait(void)
{
    swiWaitForVBlank();
    if ((gPortIE & 1) && gPortIME)
        VBlankIntr();
    ApplyDisplay();
    Pump();
    static int frame;
    if (++frame % 30 == 0)
        Status();
}

// Start
// =====

static void Init(void)
{
    // Main engine on the top screen, laid out like the GBA: BG at
    // 0x06000000, sprites at 0x06400000; palettes and OAM where the GBA has
    // them.
    lcdMainOnTop();
    videoSetMode(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankB(VRAM_B_MAIN_SPRITE_0x06400000);
    memset((void *)0x06400000, 0, 0x8000);

    // WIN0 covers the top and bottom borders, WIN1 the left and right
    // (rectangles wrap when the start is past the end); nothing shows in
    // either.
    WIN0_X0 = 0;
    WIN0_X1 = 255;
    WIN0_Y0 = 160 + OFS_Y;
    WIN0_Y1 = OFS_Y;
    WIN1_X0 = 240 + OFS_X;
    WIN1_X1 = OFS_X;
    WIN1_Y0 = 0;
    WIN1_Y1 = 192;
    WIN_IN = 0;

    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&bottom, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    memcpy(gPortCartHeader + 0xAC, "RELS", 4);
}

static void Say(const char *s)
{
    consoleSelect(&bottom);
    consoleClear();
    printf("\x1b[0;0H  Crystal Chronicles GBA link\n");
    printf("       over Sion Link\n\n%s\n", s);
}

int main(int argc, char *argv[])
{
    Init();
    cpuStartTiming(0);
    Say("Starting wireless...");
    if (SlInit() && SlJoin(Say))
        joined = true;

    // What a GameCube writes into the multiboot image before booting it;
    // Link_Init() reads it once, at start
    Say("Waiting for the FFCC host on the Sion...");
    for (u32 t0 = SlMillis(); joined && !gPortMbHeader[0xC4] && SlMillis() - t0 < 5000;)
    {
        swiWaitForVBlank();
        Pump();
    }
    consoleClear();
    setjmp(restart);
    AgbMain();
    return 0;
}
