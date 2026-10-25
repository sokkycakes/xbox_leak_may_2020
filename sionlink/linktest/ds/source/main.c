// Link Test, DS client: the Sion sends this over Download Play. It links
// back to the Sion by itself, sends its buttons and touch every frame, and
// draws the field the Sion sends back every frame. The top screen shows how
// steady the link is both ways.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>

#include "ltmeter.h"
#include "ltproto.h"
#include "sllink.h"

static PrintConsole top;

static void Status(const char *s)
{
    consoleSelect(&top);
    consoleClear();
    printf("\x1b[1;8H== Link Test ==\n\n%s\n", s);
}

// The field
// =========

static u16 *radar;                // sub screen BG3, 256x192 16-bit
static u16 back[256 * 192];

static lt_snap snap;
static lt_player players[LT_MAX_PLAYERS];
static char names[256][11];
static int my_id;

static u16 Rgb15(u32 c)
{
    return RGB15((c >> 19) & 31, (c >> 11) & 31, (c >> 3) & 31) | BIT(15);
}

static void Box(int x, int y, int w, int h, u16 c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < 256 && j >= 0 && j < 192)
                back[j * 256 + i] = c;
}

static void DrawField(void)
{
    u16 bg = RGB15(2, 3, 6) | BIT(15), grid = RGB15(4, 6, 10) | BIT(15);
    for (int i = 0; i < 256 * 192; i++)
        back[i] = bg;
    for (int x = 0; x < 256; x += 32)
        Box(x, 0, 1, 192, grid);
    for (int y = 0; y < 192; y += 32)
        Box(0, y, 256, 1, grid);
    for (int i = 0; i < snap.count; i++)
    {
        const lt_player *p = &players[i];
        int x = p->x * 256 / LT_FIELD_W, y = p->y * 192 / LT_FIELD_H;
        int r = p->id == my_id ? 5 : 3;
        if (p->id == my_id)
            Box(x - r - 1, y - r - 1, 2 * r + 3, 2 * r + 3, RGB15(31, 31, 31) | BIT(15));
        Box(x - r, y - r, 2 * r + 1, 2 * r + 1, Rgb15(lt_colors[p->id & 15]));
    }
    DC_FlushRange(back, sizeof(back));
    dmaCopy(back, radar, sizeof(back));
}

// Prints one full line of the top screen in place: redrawing the whole
// console every frame shows half-drawn text.
static void Line(int row, const char *fmt, ...)
{
    char s[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);
    printf("\x1b[%d;0H%-32.32s", row, s);
}

static void DrawStats(const lt_meter *snaps, const lt_player *me, u32 frames_sent)
{
    consoleSelect(&top);
    const lt_stats *s = &snaps->done;
    Line(0, "        == Link Test ==");
    if (my_id < 256 && names[my_id][0])
        Line(1, "You are player %d (%.10s)", my_id, names[my_id]);
    else
        Line(1, "You are player %d", my_id);
    Line(2, "%d on the Sion", snap.count);
    Line(4, "Sion -> DS, last second");
    Line(5, " frames %3u/60  lost %u", s->got, s->lost);
    Line(6, " longest gap %u ms", s->worst_gap);
    Line(7, " round trip %u ms (max %u)", s->rtt_avg, s->rtt_max);
    Line(9, "DS -> Sion, last second");
    if (me)
    {
        Line(10, " frames %3u/60  lost %u", me->seen.got, me->seen.lost);
        Line(11, " longest gap %u ms", me->seen.worst_gap);
    }
    else
    {
        Line(10, " (not on the Sion's field yet)");
        Line(11, "");
    }
    u64 all = snaps->total_got + snaps->total_lost;
    Line(13, "Whole session");
    Line(14, " got %llu of %llu frames", (unsigned long long)snaps->total_got,
         (unsigned long long)all);
    Line(15, " worst gap %lu ms, rtt max %lu", (unsigned long)snaps->total_worst,
         (unsigned long)snaps->total_rtt_max);
    Line(16, " sent %lu inputs", (unsigned long)frames_sent);
    Line(23, "D-pad/touch: move  B: quit");
}

// Main loop
// =========

// Returns false when the player quits, true to link again.
static bool Play(void)
{
    bool welcomed = false;
    u32 seq = 0;
    lt_meter meter;
    memset(&meter, 0, sizeof(meter));
    memset(&snap, 0, sizeof(snap));
    u32 last_echo = 0;
    const lt_player *me = NULL;
    bool said_hello = false, shown = false;
    int shown_count = -1;

    for (;;)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 now = SlMillis();
        if (keysDown() & KEY_B)
        {
            SlSend(SL_BYE, NULL, 0);
            for (int i = 0; i < 10; i++)
                swiWaitForVBlank();
            return false;
        }

        const sl_hdr *h;
        const u8 *body;
        while (SlNext(&h, &body))
        {
            if (h->type == SL_GAME && h->len >= sizeof(lt_snap) && body[0] == LT_SNAP)
            {
                lt_snap s;
                memcpy(&s, body, sizeof(s));
                if (s.count > LT_MAX_PLAYERS
                    || h->len < sizeof(lt_snap) + s.count * sizeof(lt_player)
                    || !lt_meter_rx(&meter, s.seq, now))
                    continue;
                snap = s;
                memcpy(players, body + sizeof(lt_snap), s.count * sizeof(lt_player));
                me = NULL;
                for (int i = 0; i < s.count; i++)
                    if (players[i].id == my_id)
                        me = &players[i];
                if (me && me->echo_stamp && me->echo_stamp != last_echo)
                {
                    last_echo = me->echo_stamp;
                    u32 rtt = now - me->echo_stamp;
                    rtt = rtt > me->echo_hold ? rtt - me->echo_hold : 0;
                    lt_meter_rtt(&meter, rtt);
                }
            }
            else if (h->type == SL_GAME && h->len >= 2 && body[0] == LT_NAMES)
            {
                lt_names n;
                memset(&n, 0, sizeof(n));
                memcpy(&n, body, h->len < sizeof(n) ? h->len : sizeof(n));
                for (int i = 0; i < n.count && i < LT_MAX_PLAYERS; i++)
                {
                    memcpy(names[n.p[i].id], n.p[i].name, 10);
                    names[n.p[i].id][10] = 0;
                }
            }
            else if (h->type == SL_BYE)
            {
                return true;
            }
        }

        my_id = SlKeepAlive(LT_APP_ID, now);
        welcomed = my_id != 0;

        if (welcomed)
        {
            lt_input in = { 0 };
            touchPosition t;
            in.kind = LT_INPUT;
            in.keys = keysHeld();
            if (in.keys & KEY_TOUCH)
            {
                touchRead(&t);
                in.touching = 1;
                in.touch_x = t.px;
                in.touch_y = t.py;
            }
            in.seq = ++seq;
            in.stamp = now;
            in.seen = meter.done;
            SlSend(SL_GAME, &in, sizeof(in));
        }

        bool second = lt_meter_tick(&meter, now);
        if (SlQuietFor(now) > 5000)
        {
            Status("Lost the Sion. Linking again...");
            for (int i = 0; i < 60; i++)
                swiWaitForVBlank();
            return true;
        }

        if (!welcomed)
        {
            if (!said_hello)
                Status("Linked. Saying hello...\n\nB: quit");
            said_hello = true;
            continue;
        }
        DrawField();
        if (second || !shown || snap.count != shown_count)
        {
            if (!shown)
                consoleClear();
            DrawStats(&meter, me, seq);
            shown = true;
            shown_count = snap.count;
        }
    }
}

int main(int argc, char *argv[])
{
    videoSetMode(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    consoleInit(&top, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    radar = bgGetGfxPtr(bg);

    cpuStartTiming(0);

    Status("Starting wireless...");
    if (!SlInit())
    {
        Status("Wireless failed to start.");
        for (;;)
            swiWaitForVBlank();
    }

    while (SlJoin(Status) && Play())
        ;

    Wifi_DisableWifi();
    return 0;
}
