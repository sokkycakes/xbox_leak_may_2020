// Sion Link for DS game clients (see sllink.h).

#include <string.h>

#include <nds.h>
#include <dswifi9.h>

#include "sllink.h"

uint32_t SlMillis(void)
{
    static u32 last;
    static u64 ticks;
    u32 t = cpuGetTiming();
    ticks += (u32)(t - last);
    last = t;
    return (u32)(ticks * 1000 / BUS_CLOCK);
}

// Packets from the IRQ handler, drained by SlNext()
// ==================================================

#define RXQ_LEN 32
#define RXQ_MAX (sizeof(sl_hdr) + SL_MAX_BODY)

static u8 rxq[RXQ_LEN][RXQ_MAX];
static volatile u16 rxq_len[RXQ_LEN];
static volatile int rxq_head, rxq_tail;
static u8 sion_mac[6], my_mac[6];
static u8 current[RXQ_MAX];

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

// Everything from the Sion is a broadcast data frame naming its DS
// (slproto.h). Runs in the IRQ handler.
static void RawFrame(int base, int len)
{
    if (len < 24 + 6)
        return;
    u8 h[24 + 6];
    Wifi_RxRawReadPacket(base, sizeof(h), h);
    u16 fc = h[0] | (h[1] << 8);
    int type = (fc >> 2) & 3, subtype = (fc >> 4) & 15;
    if (type != 2 || subtype != 0 || !(fc & 0x0200) || memcmp(h + 10, sion_mac, 6))
        return;
    static const u8 all[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (!memcmp(h + 24, my_mac, 6) || !memcmp(h + 24, all, 6))
        QueuePacket(base + 24 + 6, len - 24 - 6);
}

static u32 last_heard;
static int player;

bool SlNext(const sl_hdr **hdr, const uint8_t **body)
{
    while (rxq_tail != rxq_head)
    {
        int len = rxq_len[rxq_tail];
        memcpy(current, rxq[rxq_tail], len);
        rxq_tail = (rxq_tail + 1) % RXQ_LEN;
        const sl_hdr *h = (const sl_hdr *)current;
        if (h->magic[0] != SL_MAGIC0 || h->magic[1] != SL_MAGIC1
            || h->len > len - (int)sizeof(sl_hdr))
            continue;
        last_heard = SlMillis();
        if (h->type == SL_WELCOME && h->len >= sizeof(sl_welcome))
            player = ((const sl_welcome *)(current + sizeof(sl_hdr)))->client_id;
        *hdr = h;
        *body = current + sizeof(sl_hdr);
        return true;
    }
    return false;
}

uint32_t SlQuietFor(uint32_t now_ms)
{
    // last_heard may be a little after now_ms (taken while draining packets)
    int32_t d = (int32_t)(now_ms - last_heard);
    return d > 0 ? (uint32_t)d : 0;
}

static u16 tx_seq;

void SlSend(int type, const void *body, int len)
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

int SlKeepAlive(uint32_t app, uint32_t now)
{
    static u32 last_hello;
    if (now - last_hello > (player ? 3000u : 500u))
    {
        sl_hello hello = { 0 };
        utf16_to_utf8(hello.name, sizeof(hello.name) - 1,
                      (char16_t *)PersonalData->name, PersonalData->nameLen * 2);
        hello.app = app;
        SlSend(SL_HELLO, &hello, sizeof(hello));
        last_hello = now;
    }
    return player;
}

// Joining
// =======

bool SlInit(void)
{
    return Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY);
}

static void Say(void (*status)(const char *), const char *s)
{
    if (status)
        status(s);
}

bool SlJoin(void (*status)(const char *text))
{
    Wifi_RawSetPacketHandler(NULL);
    Wifi_SetPromiscuousMode(0);
    Wifi_IdleMode();
    Wifi_MultiplayerClientMode(0);
    while (!Wifi_LibraryModeReady())
        swiWaitForVBlank();
    Wifi_ScanMode();
    Say(status, "Looking for the Sion...\n\nB: quit");

    Wifi_AccessPoint ap;
    for (bool found = false; !found;)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B)
            return false;
        int n = Wifi_GetNumAP();
        for (int i = 0; i < n && !found; i++)
        {
            Wifi_GetAPData(i, &ap);
            found = ap.nintendo.game_id == SL_GAME_ID;
        }
    }

    Say(status, "Linking...\n\nB: quit");
    memcpy(sion_mac, ap.bssid, 6);
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, my_mac);
    rxq_head = rxq_tail = 0;
    player = 0;
    last_heard = SlMillis();
    Wifi_RawSetPacketHandler(RawFrame);
    Wifi_SetPromiscuousMode(1); // as the Sion Link app: the Sion's broadcasts need it
    Wifi_ConnectOpenAP(&ap);
    for (;;)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B)
            return false;
        int st = Wifi_AssocStatus();
        if (st == ASSOCSTATUS_ASSOCIATED)
        {
            last_heard = SlMillis();
            return true;
        }
        if (st == ASSOCSTATUS_CANNOTCONNECT)
            return SlJoin(status);
    }
}
