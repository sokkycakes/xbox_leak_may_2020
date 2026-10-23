// The Sion's Wi-Fi link (see wifilink.h). The launcher's blocking screens became a step a
// frame; it links to the first Sion it finds, as the DS's buttons belong to the Sion (the
// DS is its pad over the DSpico's USB).
#include <stdlib.h>
#include <string.h>

#include <dswifi9.h>

#include "boot.h"
#include "slboot.h"
#include "slproto.h"
#include "wifilink.h"

static wl_status st;
static wl_status reported;
static bool started;      // DSWifi is up (Wifi_InitDefault once)
static u32 frame;         // wlTick calls: the clock here, in frames
static u32 phaseAt;       // when the current state began

// Packets from the IRQ handler, drained by wlTick
// ===============================================

#define RXQ_LEN 16
#define RXQ_MAX (sizeof(sl_hdr) + SL_MAX_BODY)

static u8 rxq[RXQ_LEN][RXQ_MAX];
static volatile u16 rxq_len[RXQ_LEN];
static volatile int rxq_head, rxq_tail;

static u8 sionMac[6], myMac[6];

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

// Promiscuous mode hands every frame the radio kept to RawFrame(): the Sion's data frames
// are broadcasts naming the DS they're for (slproto.h)
static void RawFrame(int base, int len)
{
    if (len < 24 + 6)
        return;
    u8 h[24];
    Wifi_RxRawReadPacket(base, 24, h);
    u16 fc = h[0] | (h[1] << 8);
    int type = (fc >> 2) & 3, subtype = (fc >> 4) & 15;
    if (type != 2 || subtype != 0 || !(fc & 0x0200) || memcmp(h + 10, sionMac, 6))
        return;
    u8 to[6];
    Wifi_RxRawReadPacket(base + 24, 6, to);
    static const u8 all[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (!memcmp(to, myMac, 6) || !memcmp(to, all, 6))
        QueuePacket(base + 24 + 6, len - 24 - 6);
}

static u16 txSeq;

static void Send(int type, const void* body, int len)
{
    u8 buf[sizeof(sl_hdr) + SL_MAX_BODY];
    sl_hdr* h = (sl_hdr*)buf;
    h->magic[0] = SL_MAGIC0;
    h->magic[1] = SL_MAGIC1;
    h->version = SL_VERSION;
    h->type = type;
    h->seq = txSeq++;
    h->len = len;
    if (len)
        memcpy(buf + sizeof(sl_hdr), body, len);
    Wifi_MultiplayerClientToHostDataTxFrame(buf, sizeof(sl_hdr) + len);
}

// Download Play: receiving a program from the Sion
// ================================================
// It is staged in a heap block (the mirror's memory is all free while the link runs) and
// moved into place by BootNds, which needs it above where its ARM9 binary loads (it always
// is: this app's own code and data come first) and clear of the header area at the top.

static u8* stage;
static struct {
    bool active;
    u32 id, size, next, chunks, got;
    u8 status;
    bool changed;
    u32 lastAck, doneAt;
} dl;
static u8 dlHave[SL_FILE_MAX / SL_FILE_CHUNK / 8 + 1];

static void DownloadDrop(void)
{
    free(stage);
    stage = NULL;
    dl.active = false;
    st.receiving = st.booting = false;
}

static void DownloadAck(void)
{
    sl_file_ack a = { 0 };
    a.id = dl.id;
    a.next = dl.next;
    a.status = dl.status;
    u32 k0 = dl.next / SL_FILE_CHUNK;
    for (u32 i = 0; i < 32; i++)
    {
        u32 k = k0 + 1 + i;
        if (k < dl.chunks && (dlHave[k / 8] & (1 << (k % 8))))
            a.ahead |= 1u << i;
    }
    Send(SL_FILE_ACK, &a, sizeof(a));
    dl.lastAck = frame;
    dl.changed = false;
}

static void DownloadBegin(const sl_file_begin* b)
{
    if (!dl.active || b->id != dl.id)
    {
        DownloadDrop();
        memset(&dl, 0, sizeof(dl));
        memset(dlHave, 0, sizeof(dlHave));
        dl.active = true;
        dl.id = b->id;
        dl.size = b->size;
        dl.chunks = (b->size + SL_FILE_CHUNK - 1) / SL_FILE_CHUNK;
        memcpy(st.name, b->name, 32);
        st.name[32] = 0;
        st.refused = NULL;
        bool fits = b->size >= 0x200 && b->size <= SL_FILE_MAX;
        if (fits)
            stage = malloc(b->size);
        if (stage && (u32)stage + b->size > SLBOOT_STAGE_END)
        {
            free(stage);
            stage = NULL;
        }
        dl.status = stage ? SL_FILE_RECEIVING : SL_FILE_REFUSED;
        st.receiving = stage != NULL;
        st.percent = 0;
        if (!stage)
            st.refused = fits ? "not enough memory for it" : "too big for a DS";
    }
    DownloadAck();
}

static void DownloadData(const u8* body, int len)
{
    if (!dl.active || dl.status != SL_FILE_RECEIVING || len < (int)sizeof(sl_file_data))
        return;
    sl_file_data d;
    memcpy(&d, body, sizeof(d));
    u32 n = len - sizeof(d);
    u32 idx = d.offset / SL_FILE_CHUNK;
    if (d.id != dl.id || d.offset % SL_FILE_CHUNK || idx >= dl.chunks || n > dl.size - d.offset)
        return;
    if (dlHave[idx / 8] & (1 << (idx % 8)))
        return;
    memcpy(stage + d.offset, body + sizeof(d), n);
    dlHave[idx / 8] |= 1 << (idx % 8);
    dl.got++;
    while (dl.next < dl.size)
    {
        u32 k = dl.next / SL_FILE_CHUNK;
        if (!(dlHave[k / 8] & (1 << (k % 8))))
            break;
        dl.next += SL_FILE_CHUNK;
    }
    if (dl.next > dl.size)
        dl.next = dl.size;
    dl.changed = true;
    st.percent = dl.size ? (u32)((u64)dl.next * 100 / dl.size) : 0;
}

// All of it is here: check it, then tell the Sion (three acks a tenth of a second apart)
// and boot it
static void DownloadFinish(void)
{
    const char* why = BootCheck(stage, dl.size);
    if (why)
    {
        dl.status = SL_FILE_REFUSED;
        DownloadAck();
        DownloadDrop();
        st.refused = why;
        return;
    }
    dl.status = SL_FILE_DONE;
    dl.doneAt = frame;
    st.receiving = false;
    st.booting = true;
    DownloadAck();
}

static void Boot(void)
{
    Wifi_RawSetPacketHandler(NULL);
    Wifi_DisableWifi();
    for (int i = 0; i < 30; i++)
        swiWaitForVBlank();
    BootNds(stage, dl.size);
}

// The link
// ========

static bool welcomed;
static u32 lastHello, lastHeard;
static sl_hello hello;

static void Leave(void)
{
    Wifi_RawSetPacketHandler(NULL);
    Wifi_SetPromiscuousMode(0);
    Wifi_IdleMode();
}

static void Search(void)
{
    Wifi_MultiplayerClientMode(0);
    st.state = WL_SEARCHING;
    st.host[0] = st.motd[0] = st.text[0] = 0;
    welcomed = false;
    phaseAt = frame;
}

// the link went (or never came): look again after a moment
static void Drop(void)
{
    DownloadDrop();
    Leave();
    Search();
}

static void Join(const Wifi_AccessPoint* ap)
{
    memcpy(sionMac, ap->bssid, 6);
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, myMac);
    rxq_head = rxq_tail = 0;
    Wifi_RawSetPacketHandler(RawFrame);
    // our DSWifi opens the radio's RX filters with this, which the Sion's broadcasts need
    Wifi_SetPromiscuousMode(1);
    Wifi_ConnectOpenAP((Wifi_AccessPoint*)ap);
    char name[10 * 4 + 1];
    if (utf16_to_utf8(name, sizeof(name), (char16_t*)ap->nintendo.name, ap->nintendo.name_len * 2) <= 0)
        strcpy(name, "Sion");
    strncpy(st.host, name, sizeof(st.host) - 1);
    st.host[sizeof(st.host) - 1] = 0;
    st.state = WL_LINKING;
    welcomed = false;
    lastHello = 0;
    lastHeard = phaseAt = frame;
}

static void Packets(void)
{
    while (rxq_tail != rxq_head)
    {
        const u8* pkt = rxq[rxq_tail];
        int len = rxq_len[rxq_tail];
        const sl_hdr* h = (const sl_hdr*)pkt;
        const u8* body = pkt + sizeof(sl_hdr);
        if (h->magic[0] == SL_MAGIC0 && h->magic[1] == SL_MAGIC1 && h->len <= len - (int)sizeof(sl_hdr))
        {
            lastHeard = frame;
            if (h->type == SL_WELCOME && h->len >= sizeof(sl_welcome))
            {
                sl_welcome w;
                memcpy(&w, body, sizeof(w));
                w.host[sizeof(w.host) - 1] = 0;
                w.motd[sizeof(w.motd) - 1] = 0;
                strcpy(st.host, w.host);
                strcpy(st.motd, w.motd);
                welcomed = true;
                st.state = WL_LINKED;
            }
            else if (h->type == SL_TEXT)
            {
                int n = h->len < 32 ? h->len : 32;
                memcpy(st.text, body, n);
                st.text[n] = 0;
            }
            else if (h->type == SL_FILE_BEGIN && h->len >= sizeof(sl_file_begin))
            {
                sl_file_begin b;
                memcpy(&b, body, sizeof(b));
                DownloadBegin(&b);
            }
            else if (h->type == SL_FILE_DATA)
                DownloadData(body, h->len);
            else if (h->type == SL_BYE)
            {
                rxq_tail = (rxq_tail + 1) % RXQ_LEN;
                Drop();
                return;
            }
        }
        rxq_tail = (rxq_tail + 1) % RXQ_LEN;
    }
}

void wlOn(void)
{
    if (st.state != WL_OFF)
        return;
    if (!started)
    {
        utf16_to_utf8(hello.name, sizeof(hello.name) - 1, (char16_t*)PersonalData->name, PersonalData->nameLen * 2);
        if (!Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY))
        {
            st.state = WL_FAILED;
            return;
        }
        started = true;
    }
    Search();
}

void wlOff(void)
{
    if (st.state == WL_OFF || st.state == WL_FAILED)
        return;
    if (st.state == WL_LINKED)
        Send(SL_BYE, NULL, 0);
    DownloadDrop();
    // idle, not off: a radio switched off and on again heard nothing more
    Leave();
    st.state = WL_OFF;
    st.host[0] = st.motd[0] = st.text[0] = 0;
}

void wlTick(void)
{
    frame++;
    switch (st.state)
    {
    case WL_SEARCHING:
        // DSWifi needs a moment in client mode before it scans; then a look every quarter second
        if (frame - phaseAt == 10)
            Wifi_ScanMode();
        else if (frame - phaseAt > 10 && (frame - phaseAt) % 15 == 0)
        {
            int count = Wifi_GetNumAP();
            for (int i = 0; i < count; i++)
            {
                Wifi_AccessPoint ap;
                if (Wifi_GetAPData(i, &ap) >= 0 && ap.nintendo.game_id == SL_GAME_ID)
                {
                    Join(&ap);
                    break;
                }
            }
        }
        break;
    case WL_LINKING:
    case WL_LINKED:
    {
        if (st.state == WL_LINKING && !welcomed)
        {
            int status = Wifi_AssocStatus();
            if (status == ASSOCSTATUS_CANNOTCONNECT || frame - phaseAt > 30 * 60)
            {
                Drop();
                break;
            }
            if (status != ASSOCSTATUS_ASSOCIATED)
                break;
        }
        // hello until welcomed, then every 3 s as a keepalive: broadcasts from the Sion aren't
        // retried, so the WELCOME it answers with also brings back anything we missed
        if (!lastHello || frame - lastHello > (welcomed ? 180u : 30u))
        {
            Send(SL_HELLO, &hello, sizeof(hello));
            lastHello = frame;
        }
        Packets();
        if (st.state != WL_LINKED && st.state != WL_LINKING)
            break;
        if (dl.active && dl.status == SL_FILE_RECEIVING)
        {
            if (dl.got == dl.chunks)
                DownloadFinish();
            else if (dl.changed || frame - dl.lastAck >= 6)
                DownloadAck();
        }
        if (dl.active && dl.status == SL_FILE_DONE)
        {
            if (frame - dl.lastAck >= 6)
                DownloadAck();
            if (frame - dl.doneAt >= 18)
                Boot();
        }
        if (welcomed && frame - lastHeard > 5 * 60)
            Drop();
        break;
    }
    default:
        break;
    }
}

const wl_status* wlStatus(bool* changed)
{
    *changed = memcmp(&st, &reported, sizeof st) != 0;
    reported = st;
    return &st;
}
