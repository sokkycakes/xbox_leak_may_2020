// The host side of the 3DS's local wireless (see uds.h).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctrcrypto.h"
#include "uds.h"

static const uint8_t nintendo_oui[3] = { 0x00, 0x1F, 0x32 };
static const uint8_t everyone[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

enum {
    TAG_DUMMY = 20,
    TAG_NETWORK = 21,
    TAG_NODES0 = 24,
    TAG_NODES1 = 25,
};

// What the host knows about each node. Node n (1 = the host) is nodes[n - 1].
typedef struct {
    bool used;      // has a node number
    bool joining;   // authenticated, no EAPoL-Start yet
    uint8_t mac[6];
    uint64_t fcs;   // friend code seed
    uint16_t name[10]; // UTF-16
} node;

struct uds_host {
    uds_config cfg;
    uint32_t network_id;
    uint16_t attributes;
    node nodes[UDS_MAX_NODES];
    uint8_t joining_mac[UDS_MAX_NODES][6]; // authenticated, no node yet
    uint16_t seq;
    uint64_t next_beacon;
    uint64_t started;
};

static void Put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void Put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void Put64(uint8_t *p, uint64_t v) { Put32(p, v >> 32); Put32(p + 4, v); }
static uint16_t Get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint64_t Get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}
static void PutLe32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void Name16(uint16_t out[10], const char *s)
{
    memset(out, 0, 20);
    for (int i = 0; i < 10 && s && s[i]; i++)
        out[i] = (uint8_t)s[i];
}

static void NameAscii(char out[11], const uint16_t in[10])
{
    int i;
    for (i = 0; i < 10 && in[i]; i++)
        out[i] = in[i] < 0x80 ? in[i] : '?';
    out[i] = 0;
}

uds_host *UdsCreate(const uds_config *cfg)
{
    uds_host *u = calloc(1, sizeof(*u));
    u->cfg = *cfg;
    if (u->cfg.max_nodes > UDS_MAX_NODES || !u->cfg.max_nodes)
        u->cfg.max_nodes = UDS_MAX_NODES;
    FILE *r = fopen("/dev/urandom", "rb");
    if (!r || fread(&u->network_id, 4, 1, r) != 1)
        u->network_id = 0x51045104;
    if (r)
        fclose(r);
    u->attributes = 0x8000;
    node *host = &u->nodes[0];
    host->used = true;
    memcpy(host->mac, cfg->mac, 6);
    Name16(host->name, cfg->name);
    return u;
}

void UdsDestroy(uds_host *u)
{
    free(u);
}

uint32_t UdsNetworkId(uds_host *u)
{
    return u->network_id;
}

static int TotalNodes(uds_host *u)
{
    int n = 0;
    for (int i = 0; i < u->cfg.max_nodes; i++)
        n += u->nodes[i].used;
    return n;
}

static node *FindMac(uds_host *u, const uint8_t mac[6])
{
    for (int i = 1; i < u->cfg.max_nodes; i++)
        if (u->nodes[i].used && !memcmp(u->nodes[i].mac, mac, 6))
            return &u->nodes[i];
    return NULL;
}

const uint8_t *UdsNodeMac(uds_host *u, int node_id)
{
    if (node_id < 1 || node_id > u->cfg.max_nodes || !u->nodes[node_id - 1].used)
        return NULL;
    return u->nodes[node_id - 1].mac;
}

// Beacons
// =======

void UdsSsid(uds_host *u, char out[9])
{
    snprintf(out, 9, "%08X", u->network_id);
}

// The network description as the beacon carries it: the network structure
// from its OUI on (0x1F bytes), a SHA-1 over the element, the application
// data size and the (empty) application data
static int NetworkTag(uds_host *u, uint8_t *p)
{
    uint8_t *t = p;
    *t++ = 0xDD;
    *t++ = 0x1F + 20 + 1;
    uint8_t *body = t;
    memset(body, 0, 0x1F + 20 + 1);
    memcpy(body, nintendo_oui, 3);
    body[3] = TAG_NETWORK;
    Put32(body + 4, u->cfg.comm_id);
    body[8] = u->cfg.id8;
    body[9] = u->cfg.tag_unk9;
    Put16(body + 10, u->attributes);
    Put32(body + 12, u->network_id);
    body[16] = TotalNodes(u);
    body[17] = u->cfg.max_nodes;
    body[19] = u->cfg.tag_unk13;
    uint8_t hash[20];
    Sha1(body, 0x1F + 20 + 1, hash);
    memcpy(body + 0x1F, hash, 20);
    return 2 + 0x1F + 20 + 1;
}

// The node list, encrypted: MD5, a bitmask (0), then for each node slot its
// friend code seed, name and node number, all big-endian
static int NodeData(uds_host *u, uint8_t *out)
{
    int len = 0x12 + 0x1E * u->cfg.max_nodes;
    memset(out, 0, len);
    for (int i = 0; i < u->cfg.max_nodes; i++)
    {
        const node *n = &u->nodes[i];
        uint8_t *e = out + 0x12 + 0x1E * i;
        if (!n->used)
            continue;
        Put64(e, n->fcs);
        for (int c = 0; c < 10; c++)
            Put16(e + 8 + c * 2, n->name[c]);
        Put16(e + 28, i + 1);
    }
    Md5(out + 16, len - 16, out);
    uint8_t ctr[16] = { 0 };
    memcpy(ctr, u->cfg.mac, 6);
    PutLe32(ctr + 6, u->cfg.comm_id);
    ctr[10] = u->cfg.id8;
    PutLe32(ctr + 12, u->network_id);
    Aes128Ctr(u->cfg.keys->beacon, ctr, out, len);
    return len;
}

int UdsBeaconTags(uds_host *u, uint8_t *out)
{
    uint8_t *p = out;
    // An element 3DSes always send; its meaning is unknown
    static const uint8_t dummy[] = { 0xDD, 7, 0x00, 0x1F, 0x32, TAG_DUMMY, 0x0A, 0x00, 0x00 };
    memcpy(p, dummy, sizeof(dummy));
    p += sizeof(dummy);
    p += NetworkTag(u, p);

    uint8_t nodes[0x12 + 0x1E * UDS_MAX_NODES];
    int len = NodeData(u, nodes);
    for (int part = 0, off = 0; off < len; part++)
    {
        int n = len - off > 0xFA ? 0xFA : len - off;
        *p++ = 0xDD;
        *p++ = 4 + n;
        memcpy(p, nintendo_oui, 3);
        p[3] = part ? TAG_NODES1 : TAG_NODES0;
        memcpy(p + 4, nodes + off, n);
        p += 4 + n;
        off += n;
    }
    return p - out;
}

int UdsCheckBeacon(const uds_keys *k, const uint8_t mac[6], const uint8_t *ies, int len,
                   uint32_t *network_id)
{
    const uint8_t *tag = NULL;
    uint8_t nodes[2 * 0xFA];
    int n = 0;
    for (int i = 0; i + 2 <= len && i + 2 + ies[i + 1] <= len; i += 2 + ies[i + 1])
    {
        const uint8_t *e = ies + i + 2;
        int el = ies[i + 1];
        if (ies[i] != 0xDD || el < 4 || memcmp(e, nintendo_oui, 3))
            continue;
        if (e[3] == TAG_NETWORK && el >= 0x1F + 20)
            tag = e;
        else if ((e[3] == TAG_NODES0 || e[3] == TAG_NODES1) && n + el - 4 <= (int)sizeof(nodes))
        {
            memcpy(nodes + n, e + 4, el - 4);
            n += el - 4;
        }
    }
    if (!tag || n < 0x12)
        return -1;
    uint32_t comm_id = (uint32_t)tag[4] << 24 | tag[5] << 16 | tag[6] << 8 | tag[7];
    *network_id = (uint32_t)tag[12] << 24 | tag[13] << 16 | tag[14] << 8 | tag[15];
    uint8_t ctr[16] = { 0 };
    memcpy(ctr, mac, 6);
    PutLe32(ctr + 6, comm_id);
    ctr[10] = tag[8];
    PutLe32(ctr + 12, *network_id);
    Aes128Ctr(k->beacon, ctr, nodes, n);
    uint8_t md5[16];
    Md5(nodes + 16, n - 16, md5);
    return !memcmp(md5, nodes, 16);
}

int UdsBeaconBody(uds_host *u, uint8_t *out)
{
    uint8_t *p = out;
    // Timestamp (microseconds up), interval 100 TU, capabilities as a 3DS
    // sends them (ESS, short preamble, short slot time)
    uint64_t us = 900000000ull + (u->next_beacon - u->started) * 1000;
    for (int i = 0; i < 8; i++)
        *p++ = us >> (8 * i);
    *p++ = 100;
    *p++ = 0;
    *p++ = 0x31;
    *p++ = 0x04;
    // An 8-byte SSID of zeros, as 3DS beacons have
    *p++ = 0;
    *p++ = 8;
    memset(p, 0, 8);
    p += 8;
    p += UdsBeaconTags(u, p);
    return p - out;
}

void UdsTick(uds_host *u, uint64_t now)
{
    if (!u->started)
        u->started = u->next_beacon = now;
    if (now < u->next_beacon)
        return;
    u->next_beacon = now + 100;
    uint8_t b[512];
    int len = UdsBeaconBody(u, b);
    u->cfg.tx(u->cfg.ctx, UDS_BEACON, everyone, b, len);
}

bool UdsCcmpKey(uds_host *u, uint8_t key[16])
{
    if (!u->cfg.keys->have_uds_data)
        return false;
    uint8_t c[16];
    PutLe32(c, u->cfg.comm_id);
    PutLe32(c + 4, u->network_id);
    memcpy(c + 8, u->cfg.mac, 6);
    c[14] = u->cfg.id8;
    c[15] = 0;
    uint8_t ctr[16];
    Md5(c, 16, ctr);
    Md5(u->cfg.passphrase, u->cfg.passphrase_len, key);
    Aes128Ctr(u->cfg.keys->uds_data, ctr, key, 16);
    return true;
}

// Joining
// =======

static void Tx(uds_host *u, int kind, const uint8_t *dst, const uint8_t *body, int len)
{
    u->cfg.tx(u->cfg.ctx, kind, dst, body, len);
}

static int Snap(uint8_t *p, uint16_t ethertype)
{
    static const uint8_t llc[6] = { 0xAA, 0xAA, 0x03, 0, 0, 0 };
    memcpy(p, llc, 6);
    Put16(p + 6, ethertype);
    return 8;
}

void UdsAssociated(uds_host *u, const uint8_t mac[6])
{
    for (int i = 0; i < UDS_MAX_NODES; i++)
        if (!memcmp(u->joining_mac[i], mac, 6))
            return;
    for (int i = 0; i < UDS_MAX_NODES; i++)
        if (!(u->joining_mac[i][0] | u->joining_mac[i][1] | u->joining_mac[i][2]
              | u->joining_mac[i][3] | u->joining_mac[i][4] | u->joining_mac[i][5]))
        {
            memcpy(u->joining_mac[i], mac, 6);
            return;
        }
}

static void DropJoining(uds_host *u, const uint8_t mac[6])
{
    for (int i = 0; i < UDS_MAX_NODES; i++)
        if (!memcmp(u->joining_mac[i], mac, 6))
            memset(u->joining_mac[i], 0, 6);
}

// Open authentication, then association (where the radio doesn't do it)
static void Authenticate(uds_host *u, const uint8_t src[6], const uint8_t *body, int len)
{
    if (len < 6 || body[2] != 1) // only the station's first frame (sequence 1)
        return;
    if (TotalNodes(u) >= u->cfg.max_nodes && !FindMac(u, src))
        return;
    static const uint8_t auth[6] = { 0, 0, 2, 0, 0, 0 }; // open system, sequence 2, success
    Tx(u, UDS_AUTH, src, auth, sizeof(auth));

    uint8_t assoc[6 + 2 + 8];
    assoc[0] = 0x31; // capabilities
    assoc[1] = 0x04;
    assoc[2] = 0;    // success
    assoc[3] = 0;
    assoc[4] = 1;    // association ID 1 (0xC000 marks it as one)
    assoc[5] = 0xC0;
    assoc[6] = 0;    // the SSID: the network ID as 8 hex digits
    assoc[7] = 8;
    char ssid[9];
    UdsSsid(u, ssid);
    memcpy(assoc + 8, ssid, 8);
    Tx(u, UDS_ASSOC_RESP, src, assoc, sizeof(assoc));
    UdsAssociated(u, src);
}

// EAPoL-Logoff: the node number given to the station, and the node list
static void SendLogoff(uds_host *u, const uint8_t mac[6], int node_id)
{
    uint8_t f[8 + 0x298] = { 0 };
    int n = Snap(f, UDS_ETH_EAPOL);
    uint8_t *e = f + n;
    Put16(e, 0x0202);
    Put16(e + 4, node_id);
    memcpy(e + 6, mac, 6);
    e[0x12] = TotalNodes(u);
    e[0x13] = u->cfg.max_nodes;
    for (int i = 0; i < u->cfg.max_nodes; i++)
    {
        const node *nd = &u->nodes[i];
        uint8_t *ni = e + 0x18 + i * 0x28;
        if (!nd->used)
            continue;
        Put64(ni, nd->fcs);
        for (int c = 0; c < 10; c++)
            Put16(ni + 8 + c * 2, nd->name[c]);
        Put16(ni + 0x20, i + 1);
    }
    Tx(u, UDS_DATA, mac, f, sizeof(f));
}

static void EapolStart(uds_host *u, const uint8_t src[6], const uint8_t *e, int len)
{
    if (len < 0x30 || Get16(e) != 0x0201)
        return;
    if (e[4] == UDS_SPECTATOR)
    {
        // Spectators only listen to broadcasts: no node number, not listed
        DropJoining(u, src);
        SendLogoff(u, src, 0);
        return;
    }
    node *n = FindMac(u, src);
    if (!n)
    {
        bool known = false;
        for (int i = 0; i < UDS_MAX_NODES; i++)
            known |= !memcmp(u->joining_mac[i], src, 6);
        if (!known)
            return;
        for (int i = 1; i < u->cfg.max_nodes && !n; i++)
            if (!u->nodes[i].used)
                n = &u->nodes[i];
        if (!n)
            return; // full
        DropJoining(u, src);
        n->used = true;
        memcpy(n->mac, src, 6);
        n->fcs = Get64(e + 8);
        for (int c = 0; c < 10; c++)
            n->name[c] = Get16(e + 16 + c * 2);
        char name[11];
        NameAscii(name, n->name);
        int id = n - u->nodes + 1;
        SendLogoff(u, src, id);
        if (u->cfg.node)
            u->cfg.node(u->cfg.ctx, id, true, src, name);
        return;
    }
    SendLogoff(u, src, n - u->nodes + 1); // our answer got lost
}

static void RemoveNode(uds_host *u, node *n)
{
    int id = n - u->nodes + 1;
    uint8_t mac[6];
    memcpy(mac, n->mac, 6);
    memset(n, 0, sizeof(*n));
    if (u->cfg.node)
        u->cfg.node(u->cfg.ctx, id, false, mac, "");
}

void UdsLeft(uds_host *u, const uint8_t mac[6])
{
    DropJoining(u, mac);
    node *n = FindMac(u, mac);
    if (n)
        RemoveNode(u, n);
}

void UdsKick(uds_host *u, int node_id)
{
    if (node_id < 2 || node_id > u->cfg.max_nodes || !u->nodes[node_id - 1].used)
        return;
    static const uint8_t deauth[2] = { 3, 0 }; // station is leaving
    Tx(u, UDS_DEAUTH, u->nodes[node_id - 1].mac, deauth, 2);
    RemoveNode(u, &u->nodes[node_id - 1]);
}

// Data
// ====

// SNAP, then the secure data header: payload size + 14, 2 unknown bytes,
// payload size + 10, management flag, channel, sequence, destination and
// source nodes (all big-endian), then the payload
static int SecureData(uds_host *u, uint8_t *f, int dst, int src, int channel, const void *data, int len)
{
    int n = Snap(f, UDS_ETH_DATA);
    uint8_t *h = f + n;
    Put16(h, len + 14);
    h[2] = h[3] = 0;
    Put16(h + 4, len + 10);
    h[6] = 0;
    h[7] = channel;
    Put16(h + 8, u->seq++);
    Put16(h + 10, dst);
    Put16(h + 12, src);
    memcpy(h + 14, data, len);
    return n + 14 + len;
}

void UdsSend(uds_host *u, int node_id, int channel, const void *data, int len)
{
    if (len > UDS_MAX_PAYLOAD)
        return;
    const uint8_t *mac = node_id == UDS_BROADCAST ? everyone : UdsNodeMac(u, node_id);
    if (!mac)
        return;
    uint8_t f[8 + 14 + UDS_MAX_PAYLOAD];
    int n = SecureData(u, f, node_id, UDS_HOST_NODE, channel, data, len);
    Tx(u, UDS_DATA, mac, f, n);
}

static void DataFrame(uds_host *u, const uint8_t src[6], const uint8_t *body, int len)
{
    if (len < 8 || body[0] != 0xAA || body[1] != 0xAA)
        return;
    uint16_t type = Get16(body + 6);
    if (type == UDS_ETH_EAPOL)
    {
        EapolStart(u, src, body + 8, len - 8);
        return;
    }
    if (type != UDS_ETH_DATA || len < 8 + 14)
        return;
    node *n = FindMac(u, src);
    if (!n)
        return;
    const uint8_t *h = body + 8;
    int size = Get16(h) - 14;
    if (size < 0 || 8 + 14 + size > len || h[6]) // management frames aren't used
        return;
    int channel = h[7];
    int dst = Get16(h + 10);
    int from = n - u->nodes + 1;
    if (dst != UDS_HOST_NODE && dst != UDS_BROADCAST)
    {
        // For another node: the host passes it on (a broadcast reaches
        // everyone already)
        const uint8_t *to = UdsNodeMac(u, dst);
        if (to)
            Tx(u, UDS_DATA, to, body, 8 + 14 + size);
        return;
    }
    if (u->cfg.data)
        u->cfg.data(u->cfg.ctx, from, channel, h + 14, size);
}

void UdsReceive(uds_host *u, int kind, const uint8_t src[6], const uint8_t dst[6],
                const uint8_t *body, int len)
{
    if (memcmp(dst, u->cfg.mac, 6) && memcmp(dst, everyone, 6))
        return;
    switch (kind)
    {
    case UDS_AUTH:
        Authenticate(u, src, body, len);
        break;
    case UDS_DEAUTH:
        UdsLeft(u, src);
        break;
    case UDS_DATA:
        DataFrame(u, src, body, len);
        break;
    }
}

// Keys
// ====

static bool Hex16(const char *s, uint8_t out[16])
{
    for (int i = 0; i < 16; i++)
    {
        unsigned v;
        while (*s == ' ' || *s == '\t')
            s++;
        if (sscanf(s, "%2x", &v) != 1)
            return false;
        out[i] = v;
        s += 2;
    }
    return true;
}

bool UdsLoadKeys(const char *path, uds_keys *k)
{
    memset(k, 0, sizeof(*k));
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    bool iv = false;
    while (fgets(line, sizeof(line), f))
    {
        char name[64];
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || sscanf(line, " %63[a-z0-9_]", name) != 1)
            continue;
        if (!strcmp(name, "beacon_key"))
            k->have_beacon = Hex16(eq + 1, k->beacon);
        else if (!strcmp(name, "uds_data_key"))
            k->have_uds_data = Hex16(eq + 1, k->uds_data);
        else if (!strcmp(name, "dlp_key"))
            k->have_dlp = Hex16(eq + 1, k->dlp);
        else if (!strcmp(name, "dlp_iv"))
            iv = Hex16(eq + 1, k->dlp_iv);
    }
    fclose(f);
    k->have_dlp &= iv;
    return true;
}
