// Sion Link for 3DS game clients: the same calls as the DS's (dslib/sllink.h),
// over the 3DS's local wireless (UDS) network the Sion hosts (sluds.h).

#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "sllink.h"
#include "sluds.h"

uint32_t SlMillis(void)
{
    static u64 start;
    u64 now = osGetTime();
    if (!start)
        start = now;
    return (uint32_t)(now - start);
}

static udsBindContext link_bind;
static bool bound;
static u32 last_heard;
static int player;
// Room for any UDS frame: on Download Play's network the Sion also
// broadcasts title info on this channel, bigger than any Sion Link packet,
// and a frame that doesn't fit stays stuck at the head of the queue
static u8 current[UDS_DATAFRAME_MAXSIZE] __attribute__((aligned(4)));

bool SlNext(const sl_hdr **hdr, const uint8_t **body)
{
    while (bound)
    {
        size_t len = 0;
        u16 from = 0;
        if (R_FAILED(udsPullPacket(&link_bind, current, sizeof(current), &len, &from)) || !len)
            return false;
        const sl_hdr *h = (const sl_hdr *)current;
        if (from != UDS_HOST_NETWORKNODEID || len < sizeof(sl_hdr)
            || h->magic[0] != SL_MAGIC0 || h->magic[1] != SL_MAGIC1
            || h->len > len - sizeof(sl_hdr))
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
    if (bound)
        udsSendTo(UDS_HOST_NETWORKNODEID, SL_UDS_CHANNEL, UDS_SENDFLAG_Default,
                  buf, sizeof(sl_hdr) + len);
}

// The console's user name, as UTF-8
static void OwnerName(char *out, int size)
{
    u16 name[10] = { 0 };
    memset(out, 0, size);
    if (R_FAILED(cfguInit()))
        return;
    CFGU_GetConfigInfoBlk2(sizeof(name), 0x000A0000, name);
    cfguExit();
    ssize_t n = utf16_to_utf8((u8 *)out, name, size - 1);
    out[n > 0 ? n : 0] = 0;
}

int SlKeepAlive(uint32_t app, uint32_t now)
{
    static u32 last_hello;
    if (now - last_hello > (player ? 3000u : 500u))
    {
        sl_hello hello = { 0 };
        OwnerName(hello.name, sizeof(hello.name));
        hello.app = app;
        SlSend(SL_HELLO, &hello, sizeof(hello));
        last_hello = now;
    }
    return player;
}

// Joining
// =======

static bool b_quits = true;
static volatile bool woke; // back from sleep: the system dropped the network
static bool lid_closed;

static void AptEvent(APT_HookType type, void *param)
{
    if (type == APTHOOK_ONWAKEUP)
        woke = true;
}


bool SlInit(void)
{
    static aptHookCookie cookie;
    aptHook(&cookie, AptEvent, NULL);
    // Sleep drops the network, so closing the lid only turns the screens
    // off (SlRejoin), as on a DS
    aptSetSleepAllowed(false);
    ptmuInit();
    gspLcdInit();
    return R_SUCCEEDED(udsInit(0x3000, NULL));
}

// The screens go dark while the lid is closed
static void Lid(void)
{
    static u32 last;
    u32 now = SlMillis();
    if (now - last < 250)
        return;
    last = now;
    u8 open = 1;
    if (R_FAILED(PTMU_GetShellState(&open)))
        return;
    if (!open != lid_closed)
    {
        lid_closed = !open;
        if (lid_closed)
            GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
        else
            GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    }
}

bool SlRejoin(void (*status)(const char *text), uint32_t quiet_ms)
{
    Lid();
    if (!woke && SlQuietFor(SlMillis()) < quiet_ms)
        return false;
    if (woke)
    {
        // After sleep the wireless service starts over
        if (bound)
            udsUnbind(&link_bind);
        bound = false;
        udsExit();
        udsInit(0x3000, NULL);
        woke = false;
    }
    b_quits = false;
    bool ok = SlJoin(status);
    b_quits = true;
    return ok;
}

static void Say(void (*status)(const char *), const char *s)
{
    if (status)
        status(s);
}

static bool Quit(void)
{
    hidScanInput();
    return !aptMainLoop() || (b_quits && (hidKeysDown() & KEY_B));
}

bool SlJoin(void (*status)(const char *text))
{
    static u8 scan[0x4000] __attribute__((aligned(4)));
    if (bound)
    {
        udsDisconnectNetwork();
        udsUnbind(&link_bind);
        bound = false;
    }
    Say(status, b_quits ? "Looking for the Sion...\n\nB: quit" : "Looking for the Sion...");
    for (;;)
    {
        if (Quit())
            return false;
        // The Sion Link network, or Download Play's when the Sion serves
        // that too, by turns
        static bool dlp;
        dlp = !dlp;
        udsNetworkScanInfo *nets = NULL;
        size_t total = 0;
        if (R_SUCCEEDED(udsScanBeacons(scan, sizeof(scan), &nets, &total,
                                       dlp ? SL_DLP_COMM_ID : SL_UDS_COMM_ID,
                                       dlp ? SL_DLP_ID8 : SL_UDS_ID8, NULL, false)) && total)
        {
            Say(status, b_quits ? "Linking...\n\nB: quit" : "Linking...");
            udsNetworkStruct net = nets[0].network;
            free(nets);
            nets = NULL;
            const char *pass = dlp ? SL_DLP_PASSPHRASE : SL_UDS_PASSPHRASE;
            Result r = udsConnectNetwork(&net, pass, strlen(pass) + dlp,
                                         &link_bind, UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client,
                                         SL_UDS_CHANNEL, UDS_DEFAULT_RECVBUFSIZE);
            if (R_SUCCEEDED(r))
            {
                bound = true;
                player = 0;
                last_heard = SlMillis();
                return true;
            }
            Say(status, "Couldn't link, trying again...");
        }
        free(nets);
        gspWaitForVBlank();
    }
}

void SlExit(void)
{
    if (lid_closed)
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    gspLcdExit();
    ptmuExit();
    if (bound)
    {
        SlSend(SL_BYE, NULL, 0);
        udsDisconnectNetwork();
        udsUnbind(&link_bind);
        bound = false;
    }
    udsExit();
}
