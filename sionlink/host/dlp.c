// 3DS Download Play, parent side (see dlp.h).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctrcrypto.h"
#include "dlp.h"

#define HDR 16
#define FRAG 1440      // CIA bytes per fragment
#define BLOCK_FRAGS 182 // fragments per block
#define FRAGS_PER_TICK 8 // fragments sent per DlpTick to avoid overflowing radio TX queues
#define BROADCAST_MS 500
#define REPEAT_MS 100  // how often an unanswered request is repeated
#define START_AFTER_MS 4000 // distribution starts this long after the last join
#define REBOOT_PASSPHRASE "Sion Link" // 9 bytes, no NUL

enum {
    PK_BROADCAST = 1,
    PK_AUTH = 2,
    PK_START_DIST = 3,
    PK_FRAGMENT = 4,
    PK_FINISH_DIST = 5,
    PK_START_GAME = 6,
};

enum cl_state {
    CL_NONE,
    CL_AUTH,        // accepting it; waiting for its answer
    CL_ACCEPTED,    // waiting for distribution to start
    CL_START_DIST,  // asked whether it needs the CIA
    CL_SENDING,     // sending it blocks
    CL_ASKED,       // block sent, asked whether it needs more
    CL_NO_CONTENT,  // already has it; confirming
    CL_GOT_IT,      // has the CIA; telling it to get ready to start
    CL_STARTED,     // given the reboot passphrase
};

typedef struct {
    enum cl_state state;
    uint8_t seq;      // our packet counter for it, bumped at each answer
    uint8_t resp[2];  // its response ID (from its auth answer)
    uint32_t block;   // next block to send
    uint32_t frag;    // next fragment in block to send
    uint32_t asked;   // how many times we asked about blocks
    uint64_t last;    // when we last sent it something
} client;

struct dlp_host {
    uds_host *uds;
    uint32_t key;           // checksum key
    const uint8_t *cia;
    uint32_t cia_size, meta_offset;
    uint64_t title_id;
    uint16_t title_short[64], title_long[128];
    uint16_t icon[0x900];
    char name[65];
    int max_clients;
    client cl[UDS_MAX_NODES + 1]; // by node number
    uint64_t now, next_broadcast, last_join;
    bool distributing, started;
};

static void Put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void Put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t Get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t Get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t Le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// Checksum
// ========
//
// The packet's big-endian words are summed (a short tail is padded with
// zeros), then mixed with the key: a few rounds of (h >> s | h << s) ^ key,
// the round count and shift taken from the key's low bytes. The key is the
// first 4 bytes of AES-CTR (the DLP key) over zeros, with the counter the
// DLP module's IV XORed with the host's MAC repeated.

uint32_t DlpChecksum(uint32_t key, const uint8_t *p, int len)
{
    uint32_t h = 0;
    int i;
    for (i = 0; i + 4 <= len; i += 4)
        h += Get32(p + i);
    if (i < len)
    {
        uint8_t tail[4] = { 0 };
        memcpy(tail, p + i, len - i);
        h += Get32(tail);
    }
    int rounds = (key & 7) + 2;
    int shift = ((key >> 8) & 15) + 4;
    for (int r = 0; r < rounds; r++)
        h = ((h >> shift) | (h << shift)) ^ key;
    return h;
}

uint32_t DlpChecksumKey(const uds_keys *keys, const uint8_t mac[6])
{
    uint8_t ctr[16], ks[4] = { 0 };
    for (int i = 0; i < 16; i++)
        ctr[i] = keys->dlp_iv[i] ^ mac[i % 6];
    Aes128Ctr(keys->dlp, ctr, ks, 4);
    return Get32(ks);
}

// Packets
// =======

static int Header(uint8_t *p, int type, int len, int index, const uint8_t resp[3])
{
    memset(p, 0, HDR);
    p[0] = type;
    p[1] = 0x02;
    Put16(p + 4, len);
    p[6] = 2; // little-endian 2
    p[12] = index;
    memcpy(p + 13, resp, 3);
    return HDR;
}

static void Send(dlp_host *d, int node, int channel, uint8_t *p, int len)
{
    Put32(p + 8, 0);
    Put32(p + 8, DlpChecksum(d->key, p, len));
    UdsSend(d->uds, node, channel, p, len);
}

static void Resp(client *c, uint8_t out[3])
{
    out[0] = c->seq;
    out[1] = c->resp[0];
    out[2] = c->resp[1];
}

static void Broadcast(dlp_host *d)
{
    static const uint8_t resp[3] = { 0x05 };
    uint8_t p[1464];
    int icon_at = 0;

    memset(p, 0, 768);
    Header(p, PK_BROADCAST, 768, 0, resp);
    Put32(p + 16, d->title_id >> 32);
    Put32(p + 20, d->title_id);
    p[25] = 0x10;
    p[26] = 1;
    p[27] = 1;
    Put32(p + 28, FRAG * BLOCK_FRAGS);
    p[32] = d->max_clients + 1;
    Put32(p + 56, d->meta_offset);
    Put32(p + 60, d->cia_size);
    for (int i = 0; i < 64; i++)
        Put16(p + 64 + i * 2, d->title_short[i]);
    for (int i = 0; i < 128; i++)
        Put16(p + 192 + i * 2, d->title_long[i]);
    for (int i = 0; i < 0x9C; i++)
        Put16(p + 448 + i * 2, d->icon[icon_at++]);
    Send(d, UDS_BROADCAST, DLP_BROADCAST_CHANNEL, p, 768);

    for (int k = 1; k <= 3; k++)
    {
        Header(p, PK_BROADCAST, 1448, k, resp);
        for (int i = 0; i < 0x2CC; i++)
            Put16(p + HDR + i * 2, d->icon[icon_at++]);
        Send(d, UDS_BROADCAST, DLP_BROADCAST_CHANNEL, p, 1448);
    }

    // The fifth carries the system update list; none here
    memset(p, 0, 1464);
    Header(p, PK_BROADCAST, 1464, 4, resp);
    Send(d, UDS_BROADCAST, DLP_BROADCAST_CHANNEL, p, 1464);
}

static void SendAuth(dlp_host *d, int node)
{
    static const uint8_t zero[3];
    uint8_t p[0x14] = { 0 };
    Header(p, PK_AUTH, sizeof(p), 1, zero);
    Send(d, node, DLP_CLIENT_CHANNEL, p, sizeof(p));
}

static void SendStartDist(dlp_host *d, int node, client *c)
{
    uint8_t r[3], p[0x14] = { 0 };
    Resp(c, r);
    Header(p, PK_START_DIST, sizeof(p), 1, r);
    p[16] = 1;
    Send(d, node, DLP_CLIENT_CHANNEL, p, sizeof(p));
}

static bool SendBlock(dlp_host *d, int node, client *c)
{
    static const uint8_t zero[3];
    uint8_t p[28 + FRAG];
    for (int k = 0; k < FRAGS_PER_TICK && c->frag < BLOCK_FRAGS; k++, c->frag++)
    {
        uint32_t off = (c->block * BLOCK_FRAGS + c->frag) * FRAG;
        if (off >= d->cia_size)
            return true;
        int n = d->cia_size - off < FRAG ? d->cia_size - off : FRAG;
        Header(p, PK_FRAGMENT, 28 + n, 1, zero);
        p[16] = 1;
        p[17] = 0x8A; // as 3DSes send them
        p[18] = 0x4A;
        p[19] = 0xE3;
        Put32(p + 20, c->block);
        Put16(p + 24, c->frag);
        Put16(p + 26, n);
        memcpy(p + 28, d->cia + off, n);
        Send(d, node, DLP_CLIENT_CHANNEL, p, 28 + n);
    }
    return c->frag >= BLOCK_FRAGS || (c->block * BLOCK_FRAGS + c->frag) * FRAG >= d->cia_size;
}

static void SendFinishDist(dlp_host *d, int node, client *c, uint32_t seq)
{
    uint8_t r[3], p[0x18] = { 0 };
    Resp(c, r);
    Header(p, PK_FINISH_DIST, sizeof(p), 1, r);
    p[16] = 1;
    Put32(p + 20, seq);
    Send(d, node, DLP_CLIENT_CHANNEL, p, sizeof(p));
}

static void SendGetReady(dlp_host *d, int node, client *c)
{
    uint8_t r[3], p[0x18] = { 0 };
    Resp(c, r);
    Header(p, PK_START_GAME, sizeof(p), 0, r);
    p[16] = 1;  // little-endian 1
    p[20] = 9;  // little-endian 9
    Send(d, node, DLP_CLIENT_CHANNEL, p, sizeof(p));
}

static void SendStartGame(dlp_host *d, int node, client *c)
{
    uint8_t r[3], p[0x20] = { 0 };
    Resp(c, r);
    Header(p, PK_START_GAME, sizeof(p), 1, r);
    p[16] = 1;
    memcpy(p + 20, REBOOT_PASSPHRASE, 9);
    p[29] = 9;
    Send(d, node, DLP_CLIENT_CHANNEL, p, sizeof(p));
}

// The CIA
// =======
//
// Header (0x2020 bytes): sizes of the certificate chain, ticket, TMD,
// meta and content; each section starts on a 64-byte boundary, meta last.
// The meta section holds the SMDH: names at 0x8 (16 languages of 0x200
// bytes: short 0x80, long 0x100, publisher 0x80) and the 48x48 icon at
// 0x24C0.

static uint32_t Align64(uint32_t v)
{
    return (v + 63) & ~63u;
}

static const char *ParseCia(dlp_host *d)
{
    const uint8_t *c = d->cia;
    if (d->cia_size < 0x2020 || Le32(c) != 0x2020)
        return "not a CIA";
    uint32_t cert = Le32(c + 8), ticket = Le32(c + 12), tmd = Le32(c + 16), meta = Le32(c + 20);
    uint64_t content = Le32(c + 24) | ((uint64_t)Le32(c + 28) << 32);
    uint32_t off = Align64(0x2020) + Align64(cert) + Align64(ticket);
    uint32_t tmd_at = off;
    off += Align64(tmd);
    off += Align64((uint32_t)content);
    if (meta < 0x400 + 0x36C0 || off + meta > d->cia_size)
        return "the CIA has no meta section (build it with an icon)";
    d->meta_offset = off;

    // The title ID, from the TMD (after its signature)
    const uint8_t *t = c + tmd_at;
    // Signature types 0x10000-0x10005: RSA-4096, RSA-2048, ECDSA, with SHA-1
    // then SHA-256; each padded to 64 bytes
    uint32_t sig = Get32(t) - 0x10000;
    if (sig > 5)
        return "the CIA's TMD has an unknown signature type";
    int sig_len = sig % 3 == 0 ? 0x200 + 0x3C : sig % 3 == 1 ? 0x100 + 0x3C : 0x3C + 0x40;
    const uint8_t *tid = t + 4 + sig_len + 0x4C;
    d->title_id = ((uint64_t)Get32(tid) << 32) | Get32(tid + 4);
    if ((d->title_id >> 32) != 0x00040001)
        return "the CIA isn't a Download Play child (title ID 00040001xxxxxxxx)";

    const uint8_t *smdh = c + off + 0x400;
    if (memcmp(smdh, "SMDH", 4))
        return "the CIA's meta section has no SMDH";
    const uint8_t *en = smdh + 8 + 1 * 0x200; // English
    for (int i = 0; i < 64; i++)
        d->title_short[i] = en[i * 2] | (en[i * 2 + 1] << 8);
    for (int i = 0; i < 128; i++)
        d->title_long[i] = en[0x80 + i * 2] | (en[0x80 + i * 2 + 1] << 8);
    for (int i = 0; i < 0x900; i++)
        d->icon[i] = smdh[0x24C0 + i * 2] | (smdh[0x24C0 + i * 2 + 1] << 8);
    int n;
    for (n = 0; n < 64 && d->title_short[n]; n++)
        d->name[n] = d->title_short[n] < 0x80 ? d->title_short[n] : '?';
    d->name[n] = 0;
    return NULL;
}

dlp_host *DlpCreate(uds_host *uds, const uint8_t host_mac[6], const uds_keys *keys,
                    const uint8_t *cia, uint32_t cia_size, int max_clients, const char **error)
{
    dlp_host *d = calloc(1, sizeof(*d));
    d->uds = uds;
    d->cia = cia;
    d->cia_size = cia_size;
    d->max_clients = max_clients;
    d->key = keys->have_dlp ? DlpChecksumKey(keys, host_mac) : 0;
    const char *e = ParseCia(d);
    if (e)
    {
        if (error)
            *error = e;
        free(d);
        return NULL;
    }
    return d;
}

void DlpDestroy(dlp_host *d)
{
    free(d);
}

uint64_t DlpTitleId(dlp_host *d)
{
    return d->title_id;
}

const char *DlpTitleName(dlp_host *d)
{
    return d->name;
}

// Clients
// =======

void DlpNode(dlp_host *d, int node, bool joined)
{
    if (node < 2 || node > UDS_MAX_NODES)
        return;
    client *c = &d->cl[node];
    memset(c, 0, sizeof(*c));
    if (joined && !d->distributing)
    {
        c->state = CL_AUTH; // everyone is accepted
        d->last_join = d->now;
    }
}

void DlpStart(dlp_host *d)
{
    if (d->distributing)
        return;
    d->distributing = true;
    for (int n = 2; n <= UDS_MAX_NODES; n++)
        if (d->cl[n].state == CL_ACCEPTED)
        {
            d->cl[n].state = CL_START_DIST;
            d->cl[n].last = 0;
        }
}

bool DlpDone(dlp_host *d)
{
    if (!d->started)
        return false;
    for (int n = 2; n <= UDS_MAX_NODES; n++)
        if (d->cl[n].state != CL_NONE && d->cl[n].state != CL_STARTED)
            return false;
    return true;
}

void DlpData(dlp_host *d, int node, int channel, const uint8_t *p, int len)
{
    if (node < 2 || node > UDS_MAX_NODES || channel != DLP_CLIENT_CHANNEL || len < HDR
        || Get16(p + 4) != len)
        return;
    client *c = &d->cl[node];
    switch (p[0])
    {
    case PK_AUTH:
        if (c->state == CL_AUTH && len >= 0x18)
        {
            c->resp[0] = p[0x16];
            c->resp[1] = p[0x17];
            c->seq++;
            c->state = CL_ACCEPTED;
        }
        break;
    case PK_START_DIST:
        if (c->state == CL_START_DIST && len >= 0x18)
        {
            c->seq++;
            c->state = Get16(p + 0x14) == 0x20 ? CL_SENDING : CL_NO_CONTENT;
            c->block = 0;
            c->frag = 0;
            c->asked = 0;
            c->last = 0;
        }
        break;
    case PK_FINISH_DIST:
        if ((c->state == CL_ASKED || c->state == CL_NO_CONTENT) && len >= 0x1C)
        {
            bool done_block = p[0x14], needs = p[0x15];
            uint32_t seq = Get32(p + 0x16);
            c->seq++;
            c->last = 0;
            if (!needs)
                c->state = CL_GOT_IT;
            else if (seq == c->block)
            {
                c->asked++;
                c->frag = 0;
                c->state = CL_SENDING;
            }
            else if (seq + 1 == c->block && !done_block)
            {
                // It's missing fragments of the last block: send it again
                c->asked++;
                c->block--;
                c->frag = 0;
                c->state = CL_SENDING;
            }
        }
        break;
    case PK_START_GAME:
        if ((c->state == CL_GOT_IT || c->state == CL_STARTED) && len >= 0x18 && p[20] == 9)
            c->seq++;
        break;
    }
}

void DlpTick(dlp_host *d, uint64_t now)
{
    d->now = now;
    if (!d->distributing && now >= d->next_broadcast)
    {
        Broadcast(d);
        d->next_broadcast = now + BROADCAST_MS;
    }

    bool any_accepted = false, all_have_it = true, any = false;
    for (int n = 2; n <= UDS_MAX_NODES; n++)
    {
        client *c = &d->cl[n];
        if (c->state == CL_NONE)
            continue;
        any = true;
        any_accepted |= c->state == CL_ACCEPTED;
        if (c->state != CL_GOT_IT && c->state != CL_STARTED)
            all_have_it = false;
        if (c->state == CL_SENDING)
        {
            if (c->last && now - c->last < 2)
                continue;
            c->last = now;
            if (!SendBlock(d, n, c))
                continue;
            c->block++;
            c->frag = 0;
            c->state = CL_ASKED;
            c->last = 0;
        }
        if (c->last && now - c->last < REPEAT_MS)
            continue;
        c->last = now;
        switch (c->state)
        {
        case CL_AUTH:
            SendAuth(d, n);
            break;
        case CL_START_DIST:
            SendStartDist(d, n, c);
            break;
        case CL_ASKED:
            SendFinishDist(d, n, c, c->asked);
            break;
        case CL_NO_CONTENT:
            SendFinishDist(d, n, c, 0);
            break;
        case CL_GOT_IT:
            SendGetReady(d, n, c);
            break;
        case CL_STARTED:
            SendStartGame(d, n, c); // not acknowledged: repeated until we close
            break;
        default:
            break;
        }
    }

    if (!d->distributing && any_accepted && now - d->last_join >= START_AFTER_MS)
        DlpStart(d);

    // Everyone has the CIA: give them the passphrase to start it
    if (d->distributing && any && all_have_it && !d->started)
    {
        d->started = true;
        for (int n = 2; n <= UDS_MAX_NODES; n++)
            if (d->cl[n].state == CL_GOT_IT)
            {
                SendStartGame(d, n, &d->cl[n]);
                d->cl[n].state = CL_STARTED;
            }
    }
}
