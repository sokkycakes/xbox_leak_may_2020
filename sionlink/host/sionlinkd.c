// sionlinkd: the Sion side of Sion Link.
//
// Two radio back ends carry the same protocol (common/slproto.h):
//
//   --if IFACE   Real hardware. hostapd runs the access point on IFACE with
//                the Nintendo vendor element in its beacons (see
//                sion/hostapd-sionlink.conf), so it handles beacons, auth
//                and assoc. DS data frames have no LLC/SNAP header, so the
//                kernel hands them up as 802.3 frames with a length field;
//                we read and write those on a packet socket.
//
//   --melon PORT Emulator testing. We act as the whole access point in
//                software (beacons, probe/auth/assoc replies, data) and
//                trade raw DS radio frames with a melonDS built with the
//                AirMP bridge (MELONDS_AIR=127.0.0.1:PORT).
//
// Lines typed on stdin are sent to every linked DS as SL_TEXT.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "slproto.h"

#define MAX_CLIENTS 15
#define CLIENT_TIMEOUT_MS 10000

static char host_name[24] = "Sion";
static char motd[64] = "Hello from the Sion!";
static int verbose;
static bool deaf; // melon: act like the Intel, whose auth/assoc replies never reach the DS

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "sionlinkd: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *mac_str(const uint8_t *m)
{
    static char buf[4][18];
    static int n;
    char *s = buf[n++ & 3];
    snprintf(s, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return s;
}

// Clients
// =======

typedef struct {
    bool used;
    bool welcomed;
    uint8_t mac[6];
    uint8_t aid;        // association ID (soft AP) or our own number
    char name[21];
    uint64_t last_seen;
    uint16_t seq;
} client;

static client clients[MAX_CLIENTS];

static int num_linked(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        n += clients[i].used && clients[i].welcomed;
    return n;
}

static client *find_client(const uint8_t *mac, bool create)
{
    client *free_slot = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].used && !memcmp(clients[i].mac, mac, 6))
            return &clients[i];
        if (!clients[i].used && !free_slot)
            free_slot = &clients[i];
    }
    if (!create || !free_slot)
        return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    memcpy(free_slot->mac, mac, 6);
    free_slot->aid = (uint8_t)(free_slot - clients) + 1;
    free_slot->last_seen = now_ms();
    return free_slot;
}

// Back end interface
// ==================

static uint8_t ap_mac[6];
static void (*backend_send)(const uint8_t *dst, const uint8_t *payload, int len);

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Everything goes out as a broadcast naming its DS (see slproto.h)
static void send_sl(client *c, int type, const void *body, int len)
{
    uint8_t buf[6 + sizeof(sl_hdr) + SL_MAX_BODY];
    memcpy(buf, c->mac, 6);
    sl_hdr *h = (sl_hdr *)(buf + 6);
    h->magic[0] = SL_MAGIC0;
    h->magic[1] = SL_MAGIC1;
    h->version = SL_VERSION;
    h->type = type;
    h->seq = c->seq++;
    h->len = len;
    if (len)
        memcpy(buf + 6 + sizeof(sl_hdr), body, len);
    backend_send(bcast_mac, buf, 6 + sizeof(sl_hdr) + len);
}

static void send_welcome(client *c)
{
    sl_welcome w;
    memset(&w, 0, sizeof(w));
    snprintf(w.host, sizeof(w.host), "%s", host_name);
    snprintf(w.motd, sizeof(w.motd), "%s", motd);
    w.client_id = c->aid;
    w.clients = num_linked();
    send_sl(c, SL_WELCOME, &w, sizeof(w));
}

static void drop_client(client *c, const char *why)
{
    if (c->used)
        logf_("DS %s (%s) left: %s", mac_str(c->mac), c->name, why);
    c->used = false;
}

// Handles one Sion Link packet from a DS (payload after the client AID byte).
static void handle_sl(const uint8_t *mac, const uint8_t *pkt, int len)
{
    if (len < (int)sizeof(sl_hdr))
        return;
    const sl_hdr *h = (const sl_hdr *)pkt;
    if (h->magic[0] != SL_MAGIC0 || h->magic[1] != SL_MAGIC1)
        return;
    if (h->len > len - (int)sizeof(sl_hdr))
        return;
    const uint8_t *body = pkt + sizeof(sl_hdr);

    client *c = find_client(mac, h->type == SL_HELLO);
    if (!c)
        return;
    c->last_seen = now_ms();

    switch (h->type)
    {
    case SL_HELLO:
        if (h->len >= sizeof(sl_hello))
        {
            memcpy(c->name, ((const sl_hello *)body)->name, 20);
            c->name[20] = 0;
        }
        if (!c->welcomed)
            logf_("DS %s linked, owner \"%s\", #%d", mac_str(mac), c->name, c->aid);
        c->welcomed = true;
        send_welcome(c);
        break;
    case SL_PING:
        if (verbose)
            logf_("ping from %s", mac_str(mac));
        send_sl(c, SL_PONG, body, h->len);
        break;
    case SL_BYE:
        drop_client(c, "unlinked");
        break;
    }
}

static void broadcast_text(const char *s)
{
    int len = strlen(s);
    if (len > 32)
        len = 32;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].used && clients[i].welcomed)
            send_sl(&clients[i], SL_TEXT, s, len);
}

static void housekeeping(void)
{
    static uint64_t last;
    uint64_t now = now_ms();
    if (now - last < 2000)
        return;
    last = now;
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        client *c = &clients[i];
        if (!c->used)
            continue;
        if (now - c->last_seen > CLIENT_TIMEOUT_MS)
            drop_client(c, "timed out");
        else if (c->welcomed)
            send_welcome(c); // keepalive, and refreshes the linked count
    }
}

// Nintendo vendor element, shared by both back ends
// =================================================

// Fills the element body (48 bytes, without the 221/len prefix).
static void nintendo_element(uint8_t *e)
{
    memset(e, 0, 48);
    e[0] = 0x00; e[1] = 0x09; e[2] = 0xBF;          // OUI
    e[3] = 0x00;                                    // OUI type
    e[8] = 0x00; e[9] = 0x40; e[10] = 0x00; e[11] = 0x01; // fixed ID 0x00400001 as DSWifi writes it
    e[12] = (SL_GAME_ID >> 24) & 0xFF; e[13] = (SL_GAME_ID >> 16) & 0xFF;
    e[14] = (SL_GAME_ID >> 8) & 0xFF;  e[15] = SL_GAME_ID & 0xFF;
    e[18] = 24;                                     // extra data size
    e[19] = 1;                                      // beacon type
    // cmd/reply data sizes stay 0: no multiplayer transfers
    e[24] = MAX_CLIENTS + 1;                        // players max (host + clients)
    e[25] = 1 + num_linked();                       // players now
    e[26] = 1;                                      // allows connections
    const char *nm = "Sion";
    int n = strlen(nm);
    e[27] = n;                                      // name length, UTF-16 chars
    for (int i = 0; i < n; i++)
        e[28 + i * 2] = nm[i];
}

// Soft AP back end (melonDS)
// ==========================

#define MELON_CH 6
static int melon_sock = -1;
static struct sockaddr_in melon_peer;
static bool melon_have_peer;
static uint16_t melon_seqno = 0x100;

static void melon_tx(uint8_t *frame, int flen)
{
    if (!melon_have_peer)
        return;
    uint8_t buf[2100];
    // 12-byte RX header: rate (0x14 = 2 Mbit/s), channel, length incl. FCS
    memset(buf, 0, 12);
    int len = flen + 4;
    buf[8] = 0x14;
    buf[9] = MELON_CH;
    buf[10] = len & 0xFF;
    buf[11] = len >> 8;
    memcpy(buf + 12, frame, flen);
    memset(buf + 12 + flen, 0, 4); // FCS, not checked
    sendto(melon_sock, buf, 12 + len, 0, (struct sockaddr *)&melon_peer, sizeof(melon_peer));
}

static int mgmt_header(uint8_t *p, uint16_t fc, const uint8_t *da)
{
    p[0] = fc & 0xFF; p[1] = fc >> 8;
    p[2] = p[3] = 0;
    memcpy(p + 4, da, 6);
    memcpy(p + 10, ap_mac, 6);
    memcpy(p + 16, ap_mac, 6);
    p[22] = melon_seqno & 0xFF; p[23] = melon_seqno >> 8;
    melon_seqno += 0x10;
    return 24;
}

static int beacon_body(uint8_t *p)
{
    int n = 0;
    uint64_t ts = now_ms() * 1000;
    memcpy(p + n, &ts, 8); n += 8;
    p[n++] = 100; p[n++] = 0;                         // interval
    p[n++] = 0x21; p[n++] = 0x00;                     // ESS | short preamble
    p[n++] = 0; p[n++] = 4; memcpy(p + n, "Sion", 4); n += 4;
    p[n++] = 1; p[n++] = 2; p[n++] = 0x82; p[n++] = 0x84;
    p[n++] = 3; p[n++] = 1; p[n++] = MELON_CH;
    p[n++] = 5; p[n++] = 4; p[n++] = 0; p[n++] = 1; p[n++] = 0; p[n++] = 0; // TIM
    p[n++] = 221; p[n++] = 48; nintendo_element(p + n); n += 48;
    return n;
}

static void melon_beacon(void)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t f[256];
    int n = mgmt_header(f, 0x0080, bcast);
    n += beacon_body(f + n);
    melon_tx(f, n);
}

static void melon_send(const uint8_t *dst, const uint8_t *payload, int len)
{
    uint8_t f[2048];
    // Data, FromDS: addr1 = DS, addr2 = BSSID, addr3 = source (us)
    f[0] = 0x08; f[1] = 0x02; f[2] = f[3] = 0;
    memcpy(f + 4, dst, 6);
    memcpy(f + 10, ap_mac, 6);
    memcpy(f + 16, ap_mac, 6);
    f[22] = melon_seqno & 0xFF; f[23] = melon_seqno >> 8;
    melon_seqno += 0x10;
    memcpy(f + 24, payload, len);
    melon_tx(f, 24 + len);
}

static uint8_t soft_aid[MAX_CLIENTS + 1][6];

static void melon_rx(uint8_t *buf, int n)
{
    if (n < 12 + 24)
        return;
    int chan = buf[9];
    int flen = buf[10] | (buf[11] << 8);
    if (flen > n - 12)
        flen = n - 12;
    flen -= 4; // FCS
    uint8_t *f = buf + 12;
    uint16_t fc = f[0] | (f[1] << 8);
    const uint8_t *a1 = f + 4, *sa = f + 10, *bssid = f + 16;
    int type = (fc >> 2) & 3, sub = (fc >> 4) & 0xF;
    uint8_t r[256];
    int m;

    if (verbose > 1 && type != 0)
        logf_("rx fc %04x ch %d len %d a1 %s a2 %s", fc, chan, flen, mac_str(a1), mac_str(sa));
    if (chan != MELON_CH)
        return;

    if (type == 0) // management
    {
        bool to_us = !memcmp(a1, ap_mac, 6);
        switch (sub)
        {
        case 0x4: // probe request
            m = mgmt_header(r, 0x0050, sa);
            m += beacon_body(r + m);
            melon_tx(r, m);
            break;
        case 0xB: // authentication
            if (!to_us)
                break;
            if (verbose)
                logf_("auth from %s", mac_str(sa));
            if (deaf)
                break;
            m = mgmt_header(r, 0x00B0, sa);
            r[m++] = 0; r[m++] = 0;   // open system
            r[m++] = 2; r[m++] = 0;   // sequence 2
            r[m++] = 0; r[m++] = 0;   // success
            melon_tx(r, m);
            break;
        case 0x0: // association request
        {
            if (!to_us || memcmp(bssid, ap_mac, 6))
                break;
            int aid = 0;
            for (int i = 1; i <= MAX_CLIENTS; i++)
                if (!memcmp(soft_aid[i], sa, 6)) { aid = i; break; }
            for (int i = 1; i <= MAX_CLIENTS && !aid; i++)
            {
                static const uint8_t zero[6];
                if (!memcmp(soft_aid[i], zero, 6)) { aid = i; memcpy(soft_aid[i], sa, 6); }
            }
            m = mgmt_header(r, 0x0010, sa);
            r[m++] = 0x21; r[m++] = 0;                 // capability
            r[m++] = aid ? 0 : 17; r[m++] = 0;         // status (17 = AP full)
            r[m++] = aid; r[m++] = aid ? 0xC0 : 0;     // AID
            r[m++] = 1; r[m++] = 2; r[m++] = 0x82; r[m++] = 0x84;
            if (!deaf)
                melon_tx(r, m);
            logf_("DS %s associated, AID %d", mac_str(sa), aid);
            break;
        }
        case 0xA: // disassociation
        case 0xC: // deauthentication
            for (int i = 1; i <= MAX_CLIENTS; i++)
                if (!memcmp(soft_aid[i], sa, 6))
                    memset(soft_aid[i], 0, 6);
            {
                client *c = find_client(sa, false);
                if (c)
                    drop_client(c, "disassociated");
            }
            break;
        }
        return;
    }

    if (type == 2 && (fc & 0x0100) && !(fc & 0x0200)) // data, ToDS
    {
        // addr1 = BSSID, addr2 = DS, addr3 = destination
        if (memcmp(a1, ap_mac, 6))
            return;
        // DSWifi pads the 1-byte client AID to 2 bytes (struct alignment)
        if (flen > 26)
            handle_sl(sa, f + 26, flen - 26);
    }
}

static int melon_init(int port)
{
    melon_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(port) };
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(melon_sock, (struct sockaddr *)&sin, sizeof(sin)) < 0)
    {
        logf_("bind %d: %s", port, strerror(errno));
        return -1;
    }
    // A locally administered MAC for the soft AP
    static const uint8_t mac[6] = { 0x02, 0x53, 0x49, 0x4F, 0x4E, 0x01 };
    memcpy(ap_mac, mac, 6);
    backend_send = melon_send;
    logf_("soft AP %s on channel %d, waiting for melonDS on udp %d",
          mac_str(ap_mac), MELON_CH, port);
    return melon_sock;
}

static void melon_poll(void)
{
    uint8_t buf[2100];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n;
    while ((n = recvfrom(melon_sock, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr *)&from, &fl)) >= 0)
    {
        if (!melon_have_peer || memcmp(&from, &melon_peer, sizeof(from)))
        {
            melon_peer = from;
            melon_have_peer = true;
            logf_("melonDS attached from port %d", ntohs(from.sin_port));
        }
        melon_rx(buf, n);
        fl = sizeof(from);
    }
}

// Packet socket back end (real hardware, hostapd runs the AP)
// ===========================================================

static int pkt_sock = -1;
static int pkt_ifindex;

static void pkt_send(const uint8_t *dst, const uint8_t *payload, int len)
{
    uint8_t f[ETH_HLEN + SL_MAX_BODY + 16];
    memcpy(f, dst, 6);
    memcpy(f + 6, ap_mac, 6);
    // A length field instead of an EtherType: mac80211 then adds no LLC/SNAP
    // header, so the DS sees our payload right after the 802.11 header.
    f[12] = len >> 8;
    f[13] = len & 0xFF;
    memcpy(f + ETH_HLEN, payload, len);
    int flen = ETH_HLEN + len;
    if (flen < 60)
    {
        memset(f + flen, 0, 60 - flen);
        flen = 60;
    }
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_ifindex = pkt_ifindex, .sll_halen = 6 };
    memcpy(sll.sll_addr, dst, 6);
    if (sendto(pkt_sock, f, flen, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0 && verbose)
        logf_("send: %s", strerror(errno));
}

static int pkt_init(const char *ifname)
{
    pkt_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (pkt_sock < 0)
    {
        logf_("packet socket: %s", strerror(errno));
        return -1;
    }
    struct ifreq ifr = { 0 };
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(pkt_sock, SIOCGIFINDEX, &ifr) < 0)
    {
        logf_("%s: %s", ifname, strerror(errno));
        return -1;
    }
    pkt_ifindex = ifr.ifr_ifindex;
    if (ioctl(pkt_sock, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(ap_mac, ifr.ifr_hwaddr.sa_data, 6);
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL),
                               .sll_ifindex = pkt_ifindex };
    if (bind(pkt_sock, (struct sockaddr *)&sll, sizeof(sll)) < 0)
    {
        logf_("bind %s: %s", ifname, strerror(errno));
        return -1;
    }
    backend_send = pkt_send;
    logf_("listening on %s (%s)", ifname, mac_str(ap_mac));
    return pkt_sock;
}

static void pkt_poll(void)
{
    uint8_t f[2048];
    struct sockaddr_ll from;
    socklen_t fl = sizeof(from);
    int n;
    while ((n = recvfrom(pkt_sock, f, sizeof(f), MSG_DONTWAIT,
                         (struct sockaddr *)&from, &fl)) >= 0)
    {
        fl = sizeof(from);
        if (from.sll_pkttype == PACKET_OUTGOING || n < ETH_HLEN + 2 + (int)sizeof(sl_hdr))
            continue;
        const uint8_t *pl = f + ETH_HLEN;
        int plen = n - ETH_HLEN;
        uint16_t lt = (f[12] << 8) | f[13];
        if (lt <= 1500 && lt < plen)
            plen = lt; // 802.3 length field; drop any padding
        // DS direct data: [client AID][pad][sl_hdr]...
        if (pl[2] == SL_MAGIC0 && pl[3] == SL_MAGIC1)
            handle_sl(f + 6, pl + 2, plen - 2);
        else if (verbose > 1)
            logf_("other frame from %s, type/len 0x%04x", mac_str(f + 6), lt);
    }
}

// Main
// ====

static void usage(void)
{
    fprintf(stderr,
            "usage: sionlinkd (--if IFACE | --melon PORT [--deaf]) [--name NAME] [--motd TEXT] [-v]\n"
            "       sionlinkd --vendor-element   print the hostapd vendor_elements hex\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    int melon_port = 0;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--if") && i + 1 < argc)
            ifname = argv[++i];
        else if (!strcmp(argv[i], "--melon") && i + 1 < argc)
            melon_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            snprintf(host_name, sizeof(host_name), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--motd") && i + 1 < argc)
            snprintf(motd, sizeof(motd), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--deaf"))
            deaf = true;
        else if (!strcmp(argv[i], "-v"))
            verbose++;
        else if (!strcmp(argv[i], "--vendor-element"))
        {
            uint8_t e[48];
            nintendo_element(e);
            printf("dd30");
            for (int k = 0; k < 48; k++)
                printf("%02x", e[k]);
            printf("\n");
            return 0;
        }
        else
            usage();
    }
    if (!ifname == !melon_port)
        usage();

    setvbuf(stdout, NULL, _IOLBF, 0);
    int fd = ifname ? pkt_init(ifname) : melon_init(melon_port);
    if (fd < 0)
        return 1;

    bool stdin_open = true;
    uint64_t next_beacon = 0;
    for (;;)
    {
        struct pollfd pfd[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
        poll(pfd, stdin_open ? 2 : 1, 20);

        if (melon_port)
        {
            melon_poll();
            uint64_t now = now_ms();
            if (now >= next_beacon)
            {
                melon_beacon();
                next_beacon = now + 100;
            }
        }
        else
        {
            pkt_poll();
        }

        if (stdin_open && (pfd[1].revents & (POLLIN | POLLHUP)))
        {
            char line[128];
            if (!fgets(line, sizeof(line), stdin))
                stdin_open = false;
            else
            {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0])
                    broadcast_text(line);
            }
        }

        housekeeping();
    }
}
