// Sion Link for Nintendo DS: finds a Sion over DS local wireless, joins it
// and talks to it with direct data frames (see common/slproto.h).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>

#include "boot.h"
#include "slboot.h"
#include "slproto.h"

static PrintConsole top, bottom;

// Packets from the IRQ handler, drained by the main loop
// ======================================================

#define RXQ_LEN 16
#define RXQ_MAX (sizeof(sl_hdr) + SL_MAX_BODY)

static u8 rxq[RXQ_LEN][RXQ_MAX];
static volatile u16 rxq_len[RXQ_LEN];
static volatile int rxq_head, rxq_tail;

// Queues one Sion Link packet (sl_hdr onwards). Runs in the IRQ handler.
static void QueuePacket(int base, int len)
{
    if (len < (int)sizeof(sl_hdr))
        return;
    if (len > (int)RXQ_MAX)
        len = RXQ_MAX;

    int next = (rxq_head + 1) % RXQ_LEN;
    if (next == rxq_tail)
        return; // full, drop

    Wifi_RxRawReadPacket(base, (len + 1) & ~1, rxq[rxq_head]);
    rxq_len[rxq_head] = len;
    rxq_head = next;
}

// Sending
// =======

static u16 tx_seq;

static void Send(int type, const void *body, int len)
{
    u8 buf[sizeof(sl_hdr) + SL_MAX_BODY];
    sl_hdr *h = (sl_hdr *)buf;

    h->magic[0] = SL_MAGIC0;
    h->magic[1] = SL_MAGIC1;
    h->version = SL_VERSION;
    h->type = type;
    h->seq = tx_seq++;
    h->len = len;
    if (len)
        memcpy(buf + sizeof(sl_hdr), body, len);

    Wifi_MultiplayerClientToHostDataTxFrame(buf, sizeof(sl_hdr) + len);
}

static u32 Millis(void)
{
    return timerTicks2msec(cpuGetTiming());
}

// Screens
// =======

static void Title(const char *status)
{
    consoleSelect(&top);
    consoleClear();
    printf("\x1b[1;9H== Sion Link ==\n");
    printf("\x1b[3;0H%s\n", status);
}

static bool IsSion(const Wifi_AccessPoint *ap)
{
    return ap->nintendo.game_id == SL_GAME_ID;
}

// Shows nearby Sions and returns true with *out set when the user picks one.
static bool PickSion(Wifi_AccessPoint *out)
{
    Wifi_MultiplayerClientMode(0);
    while (!Wifi_LibraryModeReady())
        swiWaitForVBlank();
    Wifi_ScanMode();

    Title("Searching for a Sion...\n\nA: link   B: quit");

    int chosen = 0;
    for (;;)
    {
        swiWaitForVBlank();
        scanKeys();
        u16 keys = keysDown();
        if (keys & KEY_B)
            return false;

        Wifi_AccessPoint list[8];
        int n = 0;
        int count = Wifi_GetNumAP();
        for (int i = 0; i < count && n < 8; i++)
        {
            Wifi_GetAPData(i, &list[n]);
            if (IsSion(&list[n]))
                n++;
        }

        if (keys & KEY_UP)
            chosen--;
        if (keys & KEY_DOWN)
            chosen++;
        if (chosen >= n)
            chosen = n - 1;
        if (chosen < 0)
            chosen = 0;

        consoleSelect(&bottom);
        consoleClear();
        if (n == 0)
        {
            printf("\n  No Sion found yet.\n");
            continue;
        }

        for (int i = 0; i < n; i++)
        {
            char name[10 * 4 + 1];
            int ret = utf16_to_utf8(name, sizeof(name), list[i].nintendo.name,
                                    list[i].nintendo.name_len * 2);
            if (ret <= 0)
                strcpy(name, "Sion");
            printf("%s %.20s\n", i == chosen ? "->" : "  ", name);
            printf("   ch %d  linked %d/%d\n\n", list[i].channel,
                   list[i].nintendo.players_current - 1,
                   list[i].nintendo.players_max - 1);
        }

        if (keys & KEY_A)
        {
            *out = list[chosen];
            return true;
        }
    }
}

// Radio counters while linking
// ============================
//
// Promiscuous mode hands every frame the radio kept to RawFrame(), so the
// link screen can show whether the Sion's replies reach this DS at all.

static u8 diag_bssid[6], diag_me[6];
static u32 diag_hw_base[18];
static u32 diag_sec_start, diag_sec_last[3], diag_sec_rate[3], diag_sec_peak[3];
static volatile u32 diag_beacons, diag_auth, diag_assoc, diag_data,
                    diag_other_sion, diag_to_me, diag_total;

static void RawFrame(int base, int len)
{
    if (len < 24)
        return;

    u8 h[24];
    Wifi_RxRawReadPacket(base, 24, h);
    diag_total++;

    u16 fc = h[0] | (h[1] << 8);
    int type = (fc >> 2) & 3, subtype = (fc >> 4) & 15;
    bool to_me = memcmp(h + 4, diag_me, 6) == 0;
    bool from_sion = memcmp(h + 10, diag_bssid, 6) == 0;

    if (to_me)
        diag_to_me++;
    if (!from_sion)
        return;

    // Data from the Sion: a broadcast naming the DS it is for (slproto.h)
    if (type == 2 && subtype == 0 && (fc & 0x0200) && len >= 24 + 6)
    {
        u8 to[6];
        Wifi_RxRawReadPacket(base + 24, 6, to);
        static const u8 all[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        if (!memcmp(to, diag_me, 6) || !memcmp(to, all, 6))
            QueuePacket(base + 24 + 6, len - 24 - 6);
    }
    if (type == 2)
        diag_data++;
    else if (type == 0 && subtype == 8)
        diag_beacons++;
    else if (type == 0 && subtype == 11)
        diag_auth++;
    else if (type == 0 && subtype == 1)
        diag_assoc++;
    else
        diag_other_sion++;
}

static void ShowRadio(void)
{
    consoleSelect(&bottom);
    consoleClear();
    printf("Sion %02X:%02X:%02X:%02X:%02X:%02X\n", diag_bssid[0], diag_bssid[1],
           diag_bssid[2], diag_bssid[3], diag_bssid[4], diag_bssid[5]);
    printf("DS   %02X:%02X:%02X:%02X:%02X:%02X\n\n", diag_me[0], diag_me[1],
           diag_me[2], diag_me[3], diag_me[4], diag_me[5]);
    printf("From the Sion:\n");
    printf("  beacons     %lu\n", (unsigned long)diag_beacons);
    printf("  auth        %lu\n", (unsigned long)diag_auth);
    printf("  assoc       %lu\n", (unsigned long)diag_assoc);
    printf("  data        %lu\n", (unsigned long)diag_data);
    printf("  other       %lu\n", (unsigned long)diag_other_sion);
    printf("To this DS:   %lu\n", (unsigned long)diag_to_me);
    printf("All frames:   %lu\n", (unsigned long)diag_total);

    // Y zeroes everything, so a burst from the Sion shows up on its own
    if (keysDown() & KEY_Y)
    {
        diag_beacons = diag_auth = diag_assoc = diag_data = diag_other_sion = 0;
        diag_to_me = diag_total = 0;
        for (int i = 0; i < 18; i++)
            diag_hw_base[i] = Wifi_GetStats(WSTAT_HW_1B0 + i);
        diag_sec_peak[0] = diag_sec_peak[1] = diag_sec_peak[2] = 0;
    }

    // Per-second rates of 1B6 (bad FCS), 1B7 (received okay), 1BD
    static const int sec_idx[3] = { 6, 7, 13 };
    u32 now = Millis();
    if (now - diag_sec_start >= 1000)
    {
        for (int k = 0; k < 3; k++)
        {
            u32 v = Wifi_GetStats(WSTAT_HW_1B0 + sec_idx[k]);
            diag_sec_rate[k] = v - diag_sec_last[k];
            diag_sec_last[k] = v;
            if (diag_sec_rate[k] > diag_sec_peak[k])
                diag_sec_peak[k] = diag_sec_rate[k];
        }
        diag_sec_start = now;
    }
    printf("\nPer second  bad FCS   ok   1BD\n");
    printf("  now      %6lu %6lu %5lu\n", (unsigned long)diag_sec_rate[0],
           (unsigned long)diag_sec_rate[1], (unsigned long)diag_sec_rate[2]);
    printf("  peak     %6lu %6lu %5lu\n", (unsigned long)diag_sec_peak[0],
           (unsigned long)diag_sec_peak[1], (unsigned long)diag_sec_peak[2]);
    printf("Y: zero the counters\n");

    // The radio's own RX/TX statistics counters, registers 1B0-1C1
    printf("\nRadio counters 1B0-1C1:\n");
    for (int i = 0; i < 18; i++)
        printf("%5lu%s", (unsigned long)(Wifi_GetStats(WSTAT_HW_1B0 + i) - diag_hw_base[i]),
               (i % 6) == 5 ? "\n" : "");
}

static bool Join(Wifi_AccessPoint *ap)
{
    Title("Linking...\n\nB: cancel");

    memcpy(diag_bssid, ap->bssid, 6);
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, diag_me);
    diag_beacons = diag_auth = diag_assoc = diag_data = diag_other_sion = 0;
    diag_to_me = diag_total = 0;
    for (int i = 0; i < 18; i++)
        diag_hw_base[i] = Wifi_GetStats(WSTAT_HW_1B0 + i);
    Wifi_RawSetPacketHandler(RawFrame);
    Wifi_SetPromiscuousMode(1);

    Wifi_ConnectOpenAP(ap);

    bool ok = false;
    for (;;)
    {
        swiWaitForVBlank();
        scanKeys();
        ShowRadio();
        if (keysDown() & KEY_B)
            break;

        int status = Wifi_AssocStatus();
        if (status == ASSOCSTATUS_ASSOCIATED)
        {
            ok = true;
            break;
        }
        if (status == ASSOCSTATUS_CANNOTCONNECT)
        {
            Title("Couldn't link to that Sion.\n\nPress any button.");
            for (;;)
            {
                swiWaitForVBlank();
                scanKeys();
                if (keysDown())
                    break;
            }
            break;
        }
    }

    // RawFrame() keeps receiving the Sion's data once linked. Promiscuous
    // mode stays on too: our DSWifi then opens the radio's RX filters, which
    // may be what the Sion's broadcasts need to get through.
    if (!ok)
    {
        Wifi_SetPromiscuousMode(0);
        Wifi_RawSetPacketHandler(NULL);
    }
    return ok;
}

// Download Play: receiving a .nds from the Sion
// ==============================================

static u8 *stage; // SL_FILE_MAX bytes at SLBOOT_STAGE, kept out of the heap

static struct {
    bool active;
    u32 id, size, next, chunks, got;
    u8 status;
    bool changed;
    u32 started, last_ack;
    char name[33];
} dl;
static u8 dl_have[SL_FILE_MAX / SL_FILE_CHUNK / 8 + 1];

static void DownloadAck(u32 now)
{
    sl_file_ack a = { 0 };
    a.id = dl.id;
    a.next = dl.next;
    a.status = dl.status;
    u32 k0 = dl.next / SL_FILE_CHUNK;
    for (u32 i = 0; i < 32; i++)
    {
        u32 k = k0 + 1 + i;
        if (k < dl.chunks && (dl_have[k / 8] & (1 << (k % 8))))
            a.ahead |= 1u << i;
    }
    Send(SL_FILE_ACK, &a, sizeof(a));
    dl.last_ack = now;
    dl.changed = false;
}

static void DownloadBegin(const sl_file_begin *b, u32 now)
{
    if (!dl.active || b->id != dl.id)
    {
        memset(&dl, 0, sizeof(dl));
        memset(dl_have, 0, sizeof(dl_have));
        dl.active = true;
        dl.id = b->id;
        dl.size = b->size;
        dl.chunks = (b->size + SL_FILE_CHUNK - 1) / SL_FILE_CHUNK;
        memcpy(dl.name, b->name, 32);
        dl.started = now;
        dl.status = (stage && b->size >= 0x200 && b->size <= SL_FILE_MAX)
                    ? SL_FILE_RECEIVING : SL_FILE_REFUSED;
    }
    DownloadAck(now);
}

static void DownloadData(const u8 *body, int len)
{
    if (!dl.active || dl.status != SL_FILE_RECEIVING || len < (int)sizeof(sl_file_data))
        return;
    sl_file_data d;
    memcpy(&d, body, sizeof(d));
    u32 n = len - sizeof(d);
    u32 idx = d.offset / SL_FILE_CHUNK;
    if (d.id != dl.id || d.offset % SL_FILE_CHUNK || idx >= dl.chunks
        || n > dl.size - d.offset)
        return;
    if (dl_have[idx / 8] & (1 << (idx % 8)))
        return;
    memcpy(stage + d.offset, body + sizeof(d), n);
    dl_have[idx / 8] |= 1 << (idx % 8);
    dl.got++;
    while (dl.next < dl.size)
    {
        u32 k = dl.next / SL_FILE_CHUNK;
        if (!(dl_have[k / 8] & (1 << (k % 8))))
            break;
        dl.next += SL_FILE_CHUNK;
    }
    if (dl.next > dl.size)
        dl.next = dl.size;
    dl.changed = true;
}

static void Wait(int frames)
{
    while (frames--)
        swiWaitForVBlank();
}

// Everything is here: check it, tell the Sion, and boot it. Returns only
// if the file can't be booted.
static void DownloadFinish(u32 now)
{
    const char *why = BootCheck(stage, dl.size);
    if (why)
    {
        dl.status = SL_FILE_REFUSED;
        DownloadAck(now);
        Title("");
        printf("Got %.32s, but can't\nboot it:\n%s\n\nPress any button.", dl.name, why);
        for (;;)
        {
            swiWaitForVBlank();
            scanKeys();
            if (keysDown())
                break;
        }
        dl.active = false;
        return;
    }

    dl.status = SL_FILE_DONE;
    for (int i = 0; i < 3; i++)
    {
        DownloadAck(now);
        Wait(6);
    }
    Title("");
    printf("Booting %.32s...\n", dl.name);
    consoleSelect(&bottom);
    consoleClear();
    Wifi_RawSetPacketHandler(NULL);
    Wifi_DisableWifi();
    Wait(30);
    BootNds(stage, dl.size);
}

static void DownloadScreen(u32 now)
{
    u32 pct = dl.size ? (u32)((u64)dl.next * 100 / dl.size) : 0;
    u32 ms = now - dl.started;
    u32 kbs = ms ? (u32)((u64)dl.got * SL_FILE_CHUNK / ms) : 0; // bytes/ms = KB/s

    consoleSelect(&top);
    consoleClear();
    printf("\x1b[1;9H== Sion Link ==\n\n");
    printf("Receiving from the Sion:\n%.32s\n\n", dl.name);
    printf("%lu%%  %lu of %lu KB  %lu KB/s\n", (unsigned long)pct,
           (unsigned long)(dl.next / 1024), (unsigned long)(dl.size / 1024),
           (unsigned long)kbs);

    consoleSelect(&bottom);
    consoleClear();
    printf("\x1b[10;2H[");
    int bars = pct * 26 / 100;
    for (int i = 0; i < 26; i++)
        putchar(i < bars ? '#' : '.');
    printf("]\n\x1b[22;0HB: unlink");
}

static void Linked(void)
{
    sl_hello hello = { 0 };
    utf16_to_utf8(hello.name, sizeof(hello.name) - 1,
                  (char16_t *)PersonalData->name, PersonalData->nameLen * 2);

    sl_welcome welcome = { 0 };
    bool welcomed = false;
    u32 last_hello = 0;
    u32 last_heard = Millis();
    u32 pings = 0, pongs = 0, rtt = 0;
    char text[4][33] = { { 0 } };

    rxq_head = rxq_tail = 0;
    dl.active = false;

    for (;;)
    {
        swiWaitForVBlank();
        scanKeys();
        u16 keys = keysDown();
        u32 now = Millis();

        if (keys & KEY_B)
        {
            Send(SL_BYE, NULL, 0);
            // Give the radio a moment to send it before leaving client mode
            for (int i = 0; i < 10; i++)
                swiWaitForVBlank();
            return;
        }

        if (!welcomed && now - last_heard > 30000)
        {
            Title("The Sion didn't answer.\n\nPress any button.");
            ShowRadio();
            for (;;)
            {
                swiWaitForVBlank();
                scanKeys();
                if (keysDown())
                    return;
            }
        }

        // Hello until welcomed, then every 3 s as a keepalive. Broadcasts from
        // the Sion aren't retried, so the WELCOME it answers with also
        // refreshes anything we missed.
        if (now - last_hello > (welcomed ? 3000u : 500u))
        {
            Send(SL_HELLO, &hello, sizeof(hello));
            last_hello = now;
        }

        if (welcomed && (keys & KEY_A))
        {
            sl_ping p = { now, ++pings };
            Send(SL_PING, &p, sizeof(p));
        }

        while (rxq_tail != rxq_head)
        {
            const u8 *pkt = rxq[rxq_tail];
            int len = rxq_len[rxq_tail];
            const sl_hdr *h = (const sl_hdr *)pkt;
            const u8 *body = pkt + sizeof(sl_hdr);

            if (h->magic[0] == SL_MAGIC0 && h->magic[1] == SL_MAGIC1
                && h->len <= len - (int)sizeof(sl_hdr))
            {
                last_heard = now;
                if (h->type == SL_WELCOME && h->len >= sizeof(sl_welcome))
                {
                    memcpy(&welcome, body, sizeof(welcome));
                    welcome.host[sizeof(welcome.host) - 1] = 0;
                    welcome.motd[sizeof(welcome.motd) - 1] = 0;
                    welcomed = true;
                }
                else if (h->type == SL_PONG && h->len >= sizeof(sl_ping))
                {
                    sl_ping p;
                    memcpy(&p, body, sizeof(p));
                    rtt = now - p.stamp;
                    pongs++;
                }
                else if (h->type == SL_TEXT)
                {
                    memmove(text[0], text[1], sizeof(text[0]) * 3);
                    int n = h->len < 32 ? h->len : 32;
                    memcpy(text[3], body, n);
                    text[3][n] = 0;
                }
                else if (h->type == SL_FILE_BEGIN && h->len >= sizeof(sl_file_begin))
                {
                    sl_file_begin b;
                    memcpy(&b, body, sizeof(b));
                    DownloadBegin(&b, now);
                }
                else if (h->type == SL_FILE_DATA)
                {
                    DownloadData(body, h->len);
                }
                else if (h->type == SL_BYE)
                {
                    Title("The Sion ended the link.\n\nPress any button.");
                    for (;;)
                    {
                        swiWaitForVBlank();
                        scanKeys();
                        if (keysDown())
                            return;
                    }
                }
            }
            rxq_tail = (rxq_tail + 1) % RXQ_LEN;
        }

        if (dl.active && dl.status == SL_FILE_RECEIVING)
        {
            if (dl.got == dl.chunks)
            {
                DownloadFinish(now);
                continue;
            }
            if (dl.changed || now - dl.last_ack >= 100)
                DownloadAck(now);
        }

        if (now - last_heard > 5000 && welcomed)
        {
            Title("Lost the Sion.\n\nPress any button.");
            for (;;)
            {
                swiWaitForVBlank();
                scanKeys();
                if (keysDown())
                    return;
            }
        }

        consoleSelect(&top);
        consoleClear();
        printf("\x1b[1;9H== Sion Link ==\n\n");
        if (!welcomed)
        {
            printf("Linked. Saying hello...\n");
        }
        else
        {
            printf("Linked to %.23s\n", welcome.host);
            printf("You are DS #%d (%d linked)\n\n", welcome.client_id,
                   welcome.clients);
            printf("%.64s\n", welcome.motd);
        }

        if (!welcomed)
        {
            ShowRadio();
            continue;
        }

        if (dl.active && dl.status == SL_FILE_RECEIVING)
        {
            DownloadScreen(now);
            continue;
        }

        consoleSelect(&bottom);
        consoleClear();
        printf("Pings %lu  replies %lu\n", (unsigned long)pings,
               (unsigned long)pongs);
        if (pongs)
            printf("Round trip %lu ms\n", (unsigned long)rtt);
        printf("\nFrom the Sion:\n");
        for (int i = 0; i < 4; i++)
            printf(" %s\n", text[i]);
        printf("\x1b[22;0HA: ping   B: unlink");
    }
}

int main(int argc, char *argv[])
{
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&top, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottom, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 3, false, true);

    cpuStartTiming(0);

    // Staging area for Download Play: main RAM from SLBOOT_STAGE up, which
    // the heap must never reach. Done before anything allocates.
    if (getHeapEnd() <= (u8 *)SLBOOT_STAGE
        && reduceHeapSize(getHeapLimit() - (u8 *)SLBOOT_STAGE) == 0)
        stage = (u8 *)SLBOOT_STAGE;

    Title("Starting wireless...");
    if (!Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY))
    {
        Title("Wireless failed to start.");
        for (;;)
            swiWaitForVBlank();
    }

    for (;;)
    {
        Wifi_AccessPoint ap;
        if (!PickSion(&ap))
            break;
        if (Join(&ap))
            Linked();
        Wifi_RawSetPacketHandler(NULL);
        Wifi_SetPromiscuousMode(0);
        Wifi_IdleMode();
    }

    Wifi_DisableWifi();
    return 0;
}
