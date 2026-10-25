// Test tool: plays a 3DS joining the Sion Link network on a Linux radio, to
// try sionlinkd --if3ds with mac80211_hwsim (run-vm.sh) where there is no
// 3DS. Associate first (iw connect), then:
//
//   fake3ds IFACE HOST_MAC NETWORK_ID KEYS_FILE
//
// It works out the network's CCMP key the way a 3DS does (host/uds.c),
// installs it on IFACE, sends EAPoL-Start, waits for its node number, says
// SL_HELLO to the host and waits for SL_WELCOME. Exit status 0 on success.

#include <arpa/inet.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ctrcrypto.h"
#include "slproto.h"
#include "sluds.h"
#include "uds.h"

static int nl, family, ifindex;

static void Attr(uint8_t *m, int *len, int type, const void *d, int n)
{
    struct nlattr *a = (struct nlattr *)(m + *len);
    a->nla_type = type;
    a->nla_len = NLA_HDRLEN + n;
    memcpy((uint8_t *)a + NLA_HDRLEN, d, n);
    *len += NLA_ALIGN(a->nla_len);
}

static int Request(uint8_t *m, int len, uint8_t *reply)
{
    static int seq;
    struct nlmsghdr *h = (struct nlmsghdr *)m;
    h->nlmsg_len = len;
    h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    h->nlmsg_seq = ++seq;
    send(nl, m, len, 0);
    uint8_t buf[4096];
    for (;;)
    {
        int n = recv(nl, buf, sizeof(buf), 0);
        if (n < 0)
            return -errno;
        for (struct nlmsghdr *r = (struct nlmsghdr *)buf; NLMSG_OK(r, (unsigned)n); r = NLMSG_NEXT(r, n))
        {
            if (r->nlmsg_type == NLMSG_ERROR)
                return ((struct nlmsgerr *)NLMSG_DATA(r))->error;
            if (reply)
                memcpy(reply, r, r->nlmsg_len < 4096 ? r->nlmsg_len : 4096);
        }
    }
}

static int Header(uint8_t *m, int type, int cmd)
{
    memset(m, 0, NLMSG_HDRLEN + GENL_HDRLEN);
    ((struct nlmsghdr *)m)->nlmsg_type = type;
    ((struct genlmsghdr *)(m + NLMSG_HDRLEN))->cmd = cmd;
    ((struct genlmsghdr *)(m + NLMSG_HDRLEN))->version = 1;
    return NLMSG_HDRLEN + GENL_HDRLEN;
}

static int Key(const uint8_t *mac, const uint8_t key[16], uint8_t idx)
{
    uint8_t m[256];
    uint32_t cipher = 0x000FAC04, ifi = ifindex;
    uint32_t type = mac ? NL80211_KEYTYPE_PAIRWISE : NL80211_KEYTYPE_GROUP;
    int len = Header(m, family, NL80211_CMD_NEW_KEY);
    Attr(m, &len, NL80211_ATTR_IFINDEX, &ifi, 4);
    if (mac)
        Attr(m, &len, NL80211_ATTR_MAC, mac, 6);
    int nest = len;
    ((struct nlattr *)(m + len))->nla_type = NL80211_ATTR_KEY | NLA_F_NESTED;
    len += NLA_HDRLEN;
    Attr(m, &len, NL80211_KEY_DATA, key, 16);
    Attr(m, &len, NL80211_KEY_CIPHER, &cipher, 4);
    Attr(m, &len, NL80211_KEY_IDX, &idx, 1);
    Attr(m, &len, NL80211_KEY_TYPE, &type, 4);
    ((struct nlattr *)(m + nest))->nla_len = len - nest;
    return Request(m, len, NULL);
}

static uint64_t Ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    if (argc != 5)
    {
        fprintf(stderr, "usage: fake3ds IFACE HOST_MAC NETWORK_ID KEYS_FILE\n");
        return 2;
    }
    uint8_t host[6];
    unsigned v[6];
    sscanf(argv[2], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    for (int i = 0; i < 6; i++)
        host[i] = v[i];
    uint32_t net_id = strtoul(argv[3], NULL, 16);
    uds_keys keys;
    if (!UdsLoadKeys(argv[4], &keys) || !keys.have_uds_data)
    {
        fprintf(stderr, "fake3ds: no uds_data_key in %s\n", argv[4]);
        return 1;
    }

    // The CCMP key, as UdsCcmpKey() makes it
    uint8_t c[16], ctr[16], key[16];
    uint32_t comm = SL_UDS_COMM_ID;
    memcpy(c, &comm, 4);
    memcpy(c + 4, &net_id, 4);
    memcpy(c + 8, host, 6);
    c[14] = SL_UDS_ID8;
    c[15] = 0;
    Md5(c, 16, ctr);
    Md5(SL_UDS_PASSPHRASE, strlen(SL_UDS_PASSPHRASE), key);
    Aes128Ctr(keys.uds_data, ctr, key, 16);

    nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    uint8_t m[256], reply[4096] = { 0 };
    int len = Header(m, GENL_ID_CTRL, CTRL_CMD_GETFAMILY);
    Attr(m, &len, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
    Request(m, len, reply);
    for (int at = NLMSG_HDRLEN + GENL_HDRLEN; at < (int)((struct nlmsghdr *)reply)->nlmsg_len;)
    {
        struct nlattr *a = (struct nlattr *)(reply + at);
        if (a->nla_type == CTRL_ATTR_FAMILY_ID)
            family = *(uint16_t *)((uint8_t *)a + NLA_HDRLEN);
        at += NLA_ALIGN(a->nla_len);
    }
    ifindex = if_nametoindex(argv[1]);
    int e1 = Key(host, key, 0), e2 = Key(NULL, key, 0);
    printf("fake3ds: keys installed (%d, %d)\n", e1, e2);
    if (e1 || e2)
        return 1;

    int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = ifindex };
    bind(s, (struct sockaddr *)&sll, sizeof(sll));
    struct ifreq ifr = { 0 };
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", argv[1]);
    ioctl(s, SIOCGIFHWADDR, &ifr);
    uint8_t me[6];
    memcpy(me, ifr.ifr_hwaddr.sa_data, 6);

    // EAPoL-Start: magic 0x0201, client, node information (FCS, name)
    uint8_t f[14 + 0x300] = { 0 };
    memcpy(f, host, 6);
    memcpy(f + 6, me, 6);
    f[12] = 0x88, f[13] = 0x8E;
    uint8_t *e = f + 14;
    e[0] = 0x02, e[1] = 0x01;
    e[4] = UDS_CLIENT;
    e[15] = 0x42; // FCS
    const char *name = "FAKE3DS";
    for (int i = 0; name[i]; i++)
        e[16 + i * 2 + 1] = name[i];
    struct sockaddr_ll to = { .sll_family = AF_PACKET, .sll_ifindex = ifindex, .sll_halen = 6 };
    memcpy(to.sll_addr, host, 6);

    int node = 0;
    uint64_t next = 0, give_up = Ms() + 8000;
    bool welcomed = false;
    uint16_t seq = 0;
    while (Ms() < give_up && !welcomed)
    {
        if (Ms() >= next)
        {
            if (!node)
                sendto(s, f, 14 + 0x30, 0, (struct sockaddr *)&to, sizeof(to));
            else
            {
                // SecureData to the host on the Sion Link channel: SL_HELLO
                uint8_t d[14 + 14 + 64] = { 0 };
                memcpy(d, host, 6);
                memcpy(d + 6, me, 6);
                d[12] = 0x87, d[13] = 0x6D;
                sl_hdr hdr = { { SL_MAGIC0, SL_MAGIC1 }, SL_VERSION, SL_HELLO, seq++, sizeof(sl_hello) };
                sl_hello hello = { "fake3ds", 0 };
                int n = sizeof(hdr) + sizeof(hello);
                uint8_t *h = d + 14;
                h[0] = (n + 14) >> 8, h[1] = n + 14;
                h[4] = (n + 10) >> 8, h[5] = n + 10;
                h[7] = SL_UDS_CHANNEL;
                h[10] = 0, h[11] = 1;           // to node 1, the host
                h[12] = node >> 8, h[13] = node; // from us
                memcpy(h + 14, &hdr, sizeof(hdr));
                memcpy(h + 14 + sizeof(hdr), &hello, sizeof(hello));
                sendto(s, d, 14 + 14 + n, 0, (struct sockaddr *)&to, sizeof(to));
            }
            next = Ms() + 300;
        }
        uint8_t r[2048];
        struct sockaddr_ll from;
        socklen_t fl = sizeof(from);
        struct timeval tv = { 0, 50000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int n = recvfrom(s, r, sizeof(r), 0, (struct sockaddr *)&from, &fl);
        if (n < 14 || from.sll_pkttype == PACKET_OUTGOING || memcmp(r + 6, host, 6))
            continue;
        if (r[12] == 0x88 && r[13] == 0x8E && n >= 14 + 0x18 && r[14] == 0x02 && r[15] == 0x02)
        {
            node = (r[18] << 8) | r[19];
            printf("fake3ds: EAPoL-Logoff: node %d, %d of %d nodes\n", node, r[14 + 0x12], r[14 + 0x13]);
            next = 0;
        }
        else if (r[12] == 0x87 && r[13] == 0x6D && n >= 14 + 14 + (int)sizeof(sl_hdr))
        {
            const sl_hdr *hdr = (const sl_hdr *)(r + 28);
            printf("fake3ds: data on channel %d from node %d: Sion Link type %d\n", r[14 + 7],
                   (r[14 + 12] << 8) | r[14 + 13], hdr->type);
            if (hdr->type == SL_WELCOME)
            {
                const sl_welcome *w = (const sl_welcome *)(hdr + 1);
                printf("fake3ds: WELCOME from \"%.24s\": \"%.64s\", client %d\n", w->host, w->motd, w->client_id);
                welcomed = true;
            }
        }
    }
    puts(welcomed ? "fake3ds: linked" : "fake3ds: FAILED");
    return welcomed ? 0 : 1;
}
