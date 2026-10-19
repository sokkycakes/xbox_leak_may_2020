/*
 * Network backends for the emulated Ethernet controller (nic.c).
 *
 * "nat" puts the console on a private network, 10.0.2.0/24, behind a user-mode
 * NAT that needs no privileges and works over Wi-Fi:
 *   10.0.2.2   gateway (also reaches the host's own 127.0.0.1)
 *   10.0.2.3   DNS, forwarded to the host's resolver
 *   10.0.2.15  the console, handed out by DHCP
 * It answers ARP and DHCP itself and carries UDP through one host socket per
 * console port.  A host socket accepts replies from any address (a full-cone
 * NAT), which is what peer-to-peer Xbox Live sessions need, and it binds the
 * console's own port number when it can (3074 for Xbox Live).  Xbox Live runs
 * entirely over UDP; TCP connections are refused with a reset.
 *
 * "tunnel:HOST:PORT[:LOCALPORT]" sends every frame, raw, in a UDP datagram to
 * another xbcompat (or anything that speaks the same), which makes the two
 * one Ethernet segment: System Link across the internet.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../xbcompat.h"
#include "net.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define NET_GW     IP(10, 0, 2, 2)
#define NET_DNS    IP(10, 0, 2, 3)
#define NET_GUEST  IP(10, 0, 2, 15)
#define NET_MASK   IP(255, 255, 255, 0)

static const uint8_t gw_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x35, 0x02 };
static uint8_t guest_mac[6];
static enum { BACKEND_NAT, BACKEND_TUNNEL } backend;
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static int wake_pipe[2] = { -1, -1 };

/* ---- packet helpers ---------------------------------------------------- */

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

static uint32_t csum_add(uint32_t sum, const uint8_t *p, unsigned n)
{
    for (; n > 1; p += 2, n -= 2) sum += rd16(p);
    if (n) sum += p[0] << 8;
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Send one IPv4 datagram to the console, fragmenting it to the 1500-byte MTU. */
static bool frame_log;   /* XBCOMPAT_NET_LOG=1: one line per frame */

static void log_frame(const char *dir, const uint8_t *f, unsigned len)
{
    if (!frame_log || len < 14) return;
    unsigned type = rd16(f + 12);
    if (type == 0x0800 && len >= 34) {
        const uint8_t *ip = f + 14;
        unsigned hl = (ip[0] & 15) * 4;
        unsigned sp = 0, dp = 0;
        if ((ip[9] == 17 || ip[9] == 6) && len >= 14 + hl + 4) { sp = rd16(ip + hl); dp = rd16(ip + hl + 2); }
        xlog("net %s: IP proto %u %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u, %u bytes", dir, ip[9], ip[12], ip[13], ip[14],
             ip[15], sp, ip[16], ip[17], ip[18], ip[19], dp, len);
    } else {
        xlog("net %s: type %04x %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x, %u bytes", dir, type,
             f[6], f[7], f[8], f[9], f[10], f[11], f[0], f[1], f[2], f[3], f[4], f[5], len);
    }
}

/* A frame for the title, logged like the ones it sends. */
static void deliver(const uint8_t *f, unsigned len)
{
    log_frame("in", f, len);
    nic_receive(f, len);
}

static void to_guest_ip(uint8_t proto, uint32_t src, uint32_t dst, const uint8_t *payload, unsigned len,
                        const uint8_t *dst_mac)
{
    static uint16_t ident;
    uint8_t f[1514];
    unsigned off = 0;
    ident++;
    do {
        unsigned chunk = len - off;
        bool more = false;
        if (chunk > 1480) { chunk = 1480; more = true; }
        memcpy(f, dst_mac, 6);
        memcpy(f + 6, gw_mac, 6);
        wr16(f + 12, 0x0800);
        uint8_t *ip = f + 14;
        ip[0] = 0x45; ip[1] = 0;
        wr16(ip + 2, (uint16_t)(20 + chunk));
        wr16(ip + 4, ident);
        wr16(ip + 6, (uint16_t)((more ? 0x2000 : 0) | (off / 8)));
        ip[8] = 64; ip[9] = proto;
        wr16(ip + 10, 0);
        wr32(ip + 12, src);
        wr32(ip + 16, dst);
        wr16(ip + 10, csum_fold(csum_add(0, ip, 20)));
        memcpy(ip + 20, payload + off, chunk);
        deliver(f, 14 + 20 + chunk);
        off += chunk;
    } while (off < len);
}

static void to_guest_udp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data,
                         unsigned len, const uint8_t *dst_mac)
{
    static uint8_t u[65536];
    if (len + 8 > sizeof u) return;
    wr16(u, sport);
    wr16(u + 2, dport);
    wr16(u + 4, (uint16_t)(8 + len));
    wr16(u + 6, 0);
    memcpy(u + 8, data, len);
    uint8_t pseudo[12];
    wr32(pseudo, src); wr32(pseudo + 4, dst);
    pseudo[8] = 0; pseudo[9] = 17; wr16(pseudo + 10, (uint16_t)(8 + len));
    uint16_t c = csum_fold(csum_add(csum_add(0, pseudo, 12), u, 8 + len));
    wr16(u + 6, c ? c : 0xFFFF);
    to_guest_ip(17, src, dst, u, 8 + len, dst_mac);
}

/* ---- UDP NAT ------------------------------------------------------------ */

#define MAX_FLOWS 64
static struct flow {
    int fd;
    uint16_t guest_port;
    time_t last_used;
} flows[MAX_FLOWS];
static int nflows;
static struct sockaddr_in dns_server;

static struct flow *flow_for(uint16_t guest_port)
{
    for (int i = 0; i < nflows; i++)
        if (flows[i].guest_port == guest_port) return &flows[i];
    if (nflows == MAX_FLOWS) {   /* reuse the oldest */
        int o = 0;
        for (int i = 1; i < nflows; i++) if (flows[i].last_used < flows[o].last_used) o = i;
        close(flows[o].fd);
        flows[o] = flows[--nflows];
    }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons(guest_port) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        a.sin_port = 0;
        bind(fd, (struct sockaddr *)&a, sizeof a);
    }
    socklen_t al = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &al);
    xlog("net: console UDP port %u goes out through host port %u", guest_port, ntohs(a.sin_port));
    struct flow *f = &flows[nflows++];
    f->fd = fd;
    f->guest_port = guest_port;
    f->last_used = time(NULL);
    if (wake_pipe[1] >= 0) { char c = 0; (void)!write(wake_pipe[1], &c, 1); }
    return f;
}

static void nat_udp(uint32_t src, uint32_t dst, const uint8_t *udp, unsigned len)
{
    if (len < 8) return;
    uint16_t sport = rd16(udp), dport = rd16(udp + 2);
    unsigned ulen = rd16(udp + 4);
    if (ulen < 8 || ulen > len) return;
    const uint8_t *data = udp + 8;
    unsigned dlen = ulen - 8;
    (void)src;

    if (dport == 67) {   /* DHCP */
        extern void dhcp_request(const uint8_t *, unsigned);
        dhcp_request(data, dlen);
        return;
    }
    if (dst == 0xFFFFFFFF || (dst & ~NET_MASK) == (~NET_MASK & 0xFFFFFFFF) || (dst >> 28) == 0xE) return;   /* broadcast, multicast */

    struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(dst), .sin_port = htons(dport) };
    if (dst == NET_DNS && dport == 53) to = dns_server;
    else if (dst == NET_GW) to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if ((dst & NET_MASK) == (NET_GW & NET_MASK)) return;

    pthread_mutex_lock(&net_lock);
    struct flow *f = flow_for(sport);
    if (f) {
        f->last_used = time(NULL);
        if (sendto(f->fd, data, dlen, 0, (struct sockaddr *)&to, sizeof to) < 0)
            TRACE("net: sendto %s:%u: %s", inet_ntoa(to.sin_addr), dport, strerror(errno));
    }
    pthread_mutex_unlock(&net_lock);
}

/* Refuse a TCP connection: RST for a SYN. */
static void nat_tcp(uint32_t src, uint32_t dst, const uint8_t *tcp, unsigned len)
{
    if (len < 20) return;
    uint8_t flags = tcp[13];
    if (flags & 0x04) return;   /* never answer a reset */
    uint8_t r[20] = { 0 };
    wr16(r, rd16(tcp + 2));
    wr16(r + 2, rd16(tcp));
    uint32_t seq = rd32(tcp + 4);
    unsigned dlen = len - (tcp[12] >> 4) * 4;
    if (flags & 0x10) {           /* ACK set: RST with seq = their ack */
        wr32(r + 4, rd32(tcp + 8));
        r[13] = 0x04;
    } else {
        wr32(r + 8, seq + dlen + ((flags & 0x02) ? 1 : 0) + ((flags & 0x01) ? 1 : 0));
        r[13] = 0x14;             /* RST|ACK */
    }
    r[12] = 5 << 4;
    uint8_t pseudo[12];
    wr32(pseudo, dst); wr32(pseudo + 4, src);
    pseudo[8] = 0; pseudo[9] = 6; wr16(pseudo + 10, 20);
    wr16(r + 16, csum_fold(csum_add(csum_add(0, pseudo, 12), r, 20)));
    to_guest_ip(6, dst, src, r, 20, guest_mac);
}

static void nat_icmp(uint32_t src, uint32_t dst, const uint8_t *icmp, unsigned len)
{
    if (len < 8 || icmp[0] != 8) return;   /* echo requests to the gateway and DNS only */
    if (dst != NET_GW && dst != NET_DNS) return;
    static uint8_t r[2048];
    if (len > sizeof r) return;
    memcpy(r, icmp, len);
    r[0] = 0;
    wr16(r + 2, 0);
    wr16(r + 2, csum_fold(csum_add(0, r, len)));
    to_guest_ip(1, dst, src, r, len, guest_mac);
}

/* ---- DHCP server ------------------------------------------------------ */

void dhcp_request(const uint8_t *p, unsigned len)
{
    if (len < 240 || p[0] != 1 || rd32(p + 236) != 0x63825363) return;
    int type = 0;
    for (unsigned i = 240; i < len && p[i] != 255;) {
        if (p[i] == 0) { i++; continue; }
        if (i + 1 >= len) break;
        if (p[i] == 53 && p[i + 1] >= 1) type = p[i + 2];
        i += 2 + p[i + 1];
    }
    int reply;
    if (type == 1) reply = 2;          /* DISCOVER -> OFFER */
    else if (type == 3) reply = 5;     /* REQUEST -> ACK */
    else if (type == 8) reply = 5;     /* INFORM -> ACK */
    else return;
    uint8_t r[300] = { 0 };
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, p + 4, 4);           /* xid */
    memcpy(r + 10, p + 10, 2);         /* flags */
    if (type != 8) wr32(r + 16, NET_GUEST);
    else memcpy(r + 12, p + 12, 4);
    wr32(r + 20, NET_GW);
    memcpy(r + 28, p + 28, 16);        /* chaddr */
    wr32(r + 236, 0x63825363);
    uint8_t *o = r + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)reply;
    *o++ = 54; *o++ = 4; wr32(o, NET_GW); o += 4;
    *o++ = 51; *o++ = 4; wr32(o, 86400); o += 4;
    *o++ = 1; *o++ = 4; wr32(o, NET_MASK); o += 4;
    *o++ = 3; *o++ = 4; wr32(o, NET_GW); o += 4;
    *o++ = 6; *o++ = 4; wr32(o, NET_DNS); o += 4;
    *o++ = 255;
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    xlog("net: DHCP %s, console is 10.0.2.15", reply == 2 ? "offer" : "ack");
    to_guest_udp(NET_GW, 67, 0xFFFFFFFF, 68, r, sizeof r, bcast);
}

/* ---- frames from the console ---------------------------------------- */

static struct {   /* one IP datagram being reassembled */
    uint32_t src;
    uint16_t id;
    uint8_t proto;
    unsigned total, have;
    uint8_t data[65536];
} frag;

static void nat_ip(const uint8_t *ip, unsigned len)
{
    if (len < 20 || (ip[0] >> 4) != 4) return;
    unsigned hl = (ip[0] & 15) * 4, tl = rd16(ip + 2);
    if (hl < 20 || tl < hl || tl > len) return;
    uint32_t src = rd32(ip + 12), dst = rd32(ip + 16);
    uint16_t fo = rd16(ip + 6);
    const uint8_t *pl = ip + hl;
    unsigned plen = tl - hl;
    if (fo & 0x3FFF) {   /* a fragment */
        unsigned off = (fo & 0x1FFF) * 8;
        if (off == 0 || frag.id != rd16(ip + 4) || frag.src != src) {
            frag.src = src; frag.id = rd16(ip + 4); frag.proto = ip[9];
            frag.total = 0; frag.have = 0;
        }
        if (off + plen > sizeof frag.data) return;
        memcpy(frag.data + off, pl, plen);
        frag.have += plen;
        if (!(fo & 0x2000)) frag.total = off + plen;
        if (!frag.total || frag.have < frag.total) return;
        pl = frag.data;
        plen = frag.total;
        frag.total = 0;
    }
    switch (ip[9]) {
    case 17: nat_udp(src, dst, pl, plen); break;
    case 6: nat_tcp(src, dst, pl, plen); break;
    case 1: nat_icmp(src, dst, pl, plen); break;
    }
}

static void nat_arp(const uint8_t *a, unsigned len)
{
    if (len < 28 || rd16(a + 6) != 1) return;   /* requests */
    uint32_t want = rd32(a + 24);
    if ((want & NET_MASK) != (NET_GW & NET_MASK) || want == NET_GUEST || rd32(a + 14) == want) return;
    uint8_t f[42];
    memcpy(f, a + 8, 6);
    memcpy(f + 6, gw_mac, 6);
    wr16(f + 12, 0x0806);
    uint8_t *r = f + 14;
    wr16(r, 1); wr16(r + 2, 0x0800); r[4] = 6; r[5] = 4; wr16(r + 6, 2);
    memcpy(r + 8, gw_mac, 6);
    wr32(r + 14, want);
    memcpy(r + 18, a + 8, 6);
    memcpy(r + 24, a + 14, 4);
    deliver(f, sizeof f);
}

/* ---- tunnel ------------------------------------------------------------ */

static int tunnel_fd = -1;
static struct sockaddr_in tunnel_peer;
void net_send(const uint8_t *frame, unsigned len)
{
    log_frame("out", frame, len);
    if (backend == BACKEND_TUNNEL) {
        sendto(tunnel_fd, frame, len, 0, (struct sockaddr *)&tunnel_peer, sizeof tunnel_peer);
        return;
    }
    if (len < 14) return;
    memcpy(guest_mac, frame + 6, 6);
    switch (rd16(frame + 12)) {
    case 0x0806: nat_arp(frame + 14, len - 14); break;
    case 0x0800: nat_ip(frame + 14, len - 14); break;
    }
}

/* ---- receive thread ---------------------------------------------------- */

static void *net_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "net");
    static uint8_t buf[65536];
    for (;;) {
        struct pollfd pfd[MAX_FLOWS + 2];
        uint16_t ports[MAX_FLOWS + 2];
        int n = 0;
        pfd[n].fd = wake_pipe[0]; pfd[n].events = POLLIN; ports[n++] = 0;
        pthread_mutex_lock(&net_lock);
        if (backend == BACKEND_TUNNEL) { pfd[n].fd = tunnel_fd; pfd[n].events = POLLIN; ports[n++] = 0; }
        for (int i = 0; i < nflows; i++) { pfd[n].fd = flows[i].fd; pfd[n].events = POLLIN; ports[n++] = flows[i].guest_port; }
        pthread_mutex_unlock(&net_lock);
        if (poll(pfd, n, 1000) <= 0) continue;
        if (pfd[0].revents) { char c[64]; (void)!read(wake_pipe[0], c, sizeof c); }
        for (int i = 1; i < n; i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            for (;;) {
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                ssize_t got = recvfrom(pfd[i].fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
                if (got < 0) break;
                if (backend == BACKEND_TUNNEL) {
                    deliver(buf, (unsigned)got);
                    continue;
                }
                uint32_t src = ntohl(from.sin_addr.s_addr);
                uint16_t sport = ntohs(from.sin_port);
                if (from.sin_addr.s_addr == dns_server.sin_addr.s_addr && from.sin_port == dns_server.sin_port)
                    src = NET_DNS, sport = 53;
                else if (src == INADDR_LOOPBACK)
                    src = NET_GW;
                to_guest_udp(src, sport, NET_GUEST, ports[i], buf, (unsigned)got, guest_mac);
            }
        }
    }
    return NULL;
}

/* ---- setup ------------------------------------------------------------- */

static void find_dns(void)
{
    dns_server.sin_family = AF_INET;
    dns_server.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &dns_server.sin_addr);
    /* XBCOMPAT_DNS=IP[:PORT], e.g. tools/liveserver's DNS on 127.0.0.1:5353. */
    const char *env = getenv("XBCOMPAT_DNS");
    if (env) {
        char addr[64];
        unsigned port = 53;
        if (sscanf(env, "%63[^:]:%u", addr, &port) >= 1 && inet_pton(AF_INET, addr, &dns_server.sin_addr) == 1) {
            dns_server.sin_port = htons((uint16_t)port);
            return;
        }
    }
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return;
    char line[256], addr[64];
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "nameserver %63s", addr) == 1 && inet_pton(AF_INET, addr, &dns_server.sin_addr) == 1) break;
    fclose(f);
}

bool net_init(const char *spec, const uint8_t mac[6])
{
    memcpy(guest_mac, mac, 6);
    frame_log = getenv("XBCOMPAT_NET_LOG") != NULL;
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) < 0) return false;
    if (!strcmp(spec, "nat")) {
        backend = BACKEND_NAT;
        find_dns();
        xlog("net: NAT on 10.0.2.0/24, DNS forwarded to %s:%u", inet_ntoa(dns_server.sin_addr), ntohs(dns_server.sin_port));
    } else if (!strncmp(spec, "tunnel:", 7)) {
        char host[256];
        unsigned port = 0, lport = 0;
        if (sscanf(spec + 7, "%255[^:]:%u:%u", host, &port, &lport) < 2) {
            xlog("net: tunnel spec is tunnel:HOST:PORT[:LOCALPORT]");
            return false;
        }
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai;
        if (getaddrinfo(host, NULL, &hints, &ai) != 0) { xlog("net: cannot resolve %s", host); return false; }
        tunnel_peer = *(struct sockaddr_in *)ai->ai_addr;
        tunnel_peer.sin_port = htons((uint16_t)port);
        freeaddrinfo(ai);
        tunnel_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)(lport ? lport : port)) };
        if (bind(tunnel_fd, (struct sockaddr *)&a, sizeof a) < 0) {
            xlog("net: cannot bind UDP port %u: %s", lport ? lport : port, strerror(errno));
            return false;
        }
        backend = BACKEND_TUNNEL;
        xlog("net: tunnel to %s:%u", host, port);
    } else {
        xlog("net: unknown backend '%s' (nat, tunnel:HOST:PORT[:LOCALPORT], none)", spec);
        return false;
    }
    pthread_t th;
    pthread_create(&th, NULL, net_thread, NULL);
    return true;
}
