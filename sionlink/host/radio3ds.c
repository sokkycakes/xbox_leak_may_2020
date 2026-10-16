// The 3DS network on the Sion's own radio: see radio3ds.h.

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "radio3ds.h"

#define CCMP_CIPHER 0x000FAC04

struct radio3ds {
    char ifname[IFNAMSIZ];
    char hostapd[256], run_dir[64];
    int channel, group_key_idx;
    void (*log)(const char *fmt, ...);
    int ifindex;
    uint8_t mac[6];
    int pkt;            // packet socket
    int nl, nl80211;    // generic netlink socket, nl80211's family ID
    pid_t hostapd_pid;
    int ev, cmd;        // hostapd control sockets: events (attached), commands
    char ev_path[256], cmd_path[256];
    uint64_t retry_at;  // when to try attaching again
    bool enabled;
    uds_host *u;
    uint8_t tags[512];  // the vendor elements hostapd has
    int tags_len;
};

static uint64_t Ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static const char *Mac(const uint8_t *m)
{
    static char s[2][18];
    static int i;
    i ^= 1;
    snprintf(s[i], sizeof(s[i]), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return s[i];
}

// nl80211
// =======

static void Attr(uint8_t *msg, int *len, int type, const void *data, int n)
{
    struct nlattr *a = (struct nlattr *)(msg + *len);
    a->nla_type = type;
    a->nla_len = NLA_HDRLEN + n;
    memcpy((uint8_t *)a + NLA_HDRLEN, data, n);
    *len += NLA_ALIGN(a->nla_len);
}

static int NestStart(uint8_t *msg, int *len, int type)
{
    int at = *len;
    struct nlattr *a = (struct nlattr *)(msg + at);
    a->nla_type = type | NLA_F_NESTED;
    *len += NLA_HDRLEN;
    return at;
}

static void NestEnd(uint8_t *msg, int len, int at)
{
    ((struct nlattr *)(msg + at))->nla_len = len - at;
}

// Sends a generic netlink request and waits for its acknowledgement:
// returns 0 or a negative errno
static int NlRequest(radio3ds *r, uint8_t *msg, int len, uint8_t *reply, int reply_size)
{
    static uint32_t seq;
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_len = len;
    h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    h->nlmsg_seq = ++seq;
    if (send(r->nl, msg, len, 0) < 0)
        return -errno;
    uint8_t buf[4096];
    for (;;)
    {
        int n = recv(r->nl, buf, sizeof(buf), 0);
        if (n < 0)
            return -errno;
        for (struct nlmsghdr *m = (struct nlmsghdr *)buf; NLMSG_OK(m, (unsigned)n); m = NLMSG_NEXT(m, n))
        {
            if (m->nlmsg_seq != seq)
                continue;
            if (m->nlmsg_type == NLMSG_ERROR)
                return ((struct nlmsgerr *)NLMSG_DATA(m))->error;
            if (reply && (int)m->nlmsg_len <= reply_size)
                memcpy(reply, m, m->nlmsg_len);
        }
    }
}

static int GenlHeader(uint8_t *msg, int type, int cmd)
{
    memset(msg, 0, NLMSG_HDRLEN + GENL_HDRLEN);
    ((struct nlmsghdr *)msg)->nlmsg_type = type;
    struct genlmsghdr *g = (struct genlmsghdr *)(msg + NLMSG_HDRLEN);
    g->cmd = cmd;
    g->version = 1;
    return NLMSG_HDRLEN + GENL_HDRLEN;
}

static bool NlOpen(radio3ds *r)
{
    r->nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (r->nl < 0)
        return false;
    struct timeval tv = { 1, 0 };
    setsockopt(r->nl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t msg[256], reply[4096];
    int len = GenlHeader(msg, GENL_ID_CTRL, CTRL_CMD_GETFAMILY);
    Attr(msg, &len, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
    memset(reply, 0, sizeof(reply));
    if (NlRequest(r, msg, len, reply, sizeof(reply)) < 0)
        return false;
    struct nlmsghdr *h = (struct nlmsghdr *)reply;
    int at = NLMSG_HDRLEN + GENL_HDRLEN;
    while (at + NLA_HDRLEN <= (int)h->nlmsg_len)
    {
        struct nlattr *a = (struct nlattr *)(reply + at);
        if (a->nla_len < NLA_HDRLEN)
            break;
        if (a->nla_type == CTRL_ATTR_FAMILY_ID)
            r->nl80211 = *(uint16_t *)((uint8_t *)a + NLA_HDRLEN);
        at += NLA_ALIGN(a->nla_len);
    }
    return r->nl80211 != 0;
}

// Installs the network's CCMP key: for one station (pairwise, index 0), or
// for broadcasts (sta NULL: group, made the default multicast key)
static void InstallKey(radio3ds *r, const uint8_t *sta)
{
    uint8_t key[16];
    if (!r->u || !UdsCcmpKey(r->u, key))
    {
        r->log("3DS radio: no CCMP key (uds_data_key missing from the keys file)");
        return;
    }
    uint8_t msg[256];
    uint32_t cipher = CCMP_CIPHER;
    uint32_t ifindex = r->ifindex;
    uint8_t idx = sta ? 0 : r->group_key_idx;
    uint32_t type = sta ? NL80211_KEYTYPE_PAIRWISE : NL80211_KEYTYPE_GROUP;
    int dlen = GenlHeader(msg, r->nl80211, NL80211_CMD_DEL_KEY);
    Attr(msg, &dlen, NL80211_ATTR_IFINDEX, &ifindex, 4);
    if (sta)
        Attr(msg, &dlen, NL80211_ATTR_MAC, sta, 6);
    Attr(msg, &dlen, NL80211_ATTR_KEY_IDX, &idx, 1);
    NlRequest(r, msg, dlen, NULL, 0);
    int len = GenlHeader(msg, r->nl80211, NL80211_CMD_NEW_KEY);
    Attr(msg, &len, NL80211_ATTR_IFINDEX, &ifindex, 4);
    if (sta)
        Attr(msg, &len, NL80211_ATTR_MAC, sta, 6);
    int nest = NestStart(msg, &len, NL80211_ATTR_KEY);
    Attr(msg, &len, NL80211_KEY_DATA, key, 16);
    Attr(msg, &len, NL80211_KEY_CIPHER, &cipher, 4);
    Attr(msg, &len, NL80211_KEY_IDX, &idx, 1);
    Attr(msg, &len, NL80211_KEY_TYPE, &type, 4);
    NestEnd(msg, len, nest);
    int err = NlRequest(r, msg, len, NULL, 0);
    if (err < 0)
    {
        r->log("3DS radio: installing the %s key%s%s: %s", sta ? "pairwise" : "group", sta ? " for " : "",
               sta ? Mac(sta) : "", strerror(-err));
        return;
    }
    if (sta)
        return;
    len = GenlHeader(msg, r->nl80211, NL80211_CMD_SET_KEY);
    Attr(msg, &len, NL80211_ATTR_IFINDEX, &ifindex, 4);
    nest = NestStart(msg, &len, NL80211_ATTR_KEY);
    Attr(msg, &len, NL80211_KEY_IDX, &idx, 1);
    Attr(msg, &len, NL80211_KEY_DEFAULT, NULL, 0);
    int types = NestStart(msg, &len, NL80211_KEY_DEFAULT_TYPES);
    Attr(msg, &len, NL80211_KEY_DEFAULT_TYPE_MULTICAST, NULL, 0);
    NestEnd(msg, len, types);
    NestEnd(msg, len, nest);
    err = NlRequest(r, msg, len, NULL, 0);
    if (err < 0)
        r->log("3DS radio: making key %d the broadcast key: %s", idx, strerror(-err));
}

// hostapd
// =======

static void Hex(char *out, const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        sprintf(out + i * 2, "%02x", p[i]);
    out[n * 2] = 0;
}

static void CloseCtrl(radio3ds *r)
{
    if (r->ev >= 0)
        close(r->ev);
    if (r->cmd >= 0)
        close(r->cmd);
    r->ev = r->cmd = -1;
    unlink(r->ev_path);
    unlink(r->cmd_path);
}

static void StopHostapd(radio3ds *r)
{
    CloseCtrl(r);
    if (r->hostapd_pid > 0)
    {
        kill(r->hostapd_pid, SIGTERM);
        for (int i = 0; i < 50 && waitpid(r->hostapd_pid, NULL, WNOHANG) == 0; i++)
            usleep(20000);
        kill(r->hostapd_pid, SIGKILL);
        waitpid(r->hostapd_pid, NULL, 0);
    }
    r->hostapd_pid = 0;
    r->enabled = false;
}

static int CtrlSocket(radio3ds *r, char *path, const char *what)
{
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(path, sizeof(a.sun_path), "%s/sionlinkd-%s-%d", r->run_dir, what, (int)getpid());
    unlink(path);
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    struct sockaddr_un to = { .sun_family = AF_UNIX };
    snprintf(to.sun_path, sizeof(to.sun_path), "%s/hostapd3ds/%s", r->run_dir, r->ifname);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || connect(s, (struct sockaddr *)&to, sizeof(to)) < 0)
    {
        close(s);
        unlink(path);
        return -1;
    }
    return s;
}

// A command for hostapd; returns its answer's first line, or NULL
static const char *Command(radio3ds *r, const char *fmt, ...)
{
    static char buf[4096];
    if (r->cmd < 0)
        return NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof(buf) || send(r->cmd, buf, n, 0) < 0)
        return NULL;
    struct timeval tv = { 1, 0 };
    setsockopt(r->cmd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    n = recv(r->cmd, buf, sizeof(buf) - 1, 0);
    if (n < 0)
        return NULL;
    buf[n] = 0;
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = 0;
    return buf;
}

static void Attach(radio3ds *r)
{
    if (r->cmd < 0)
        r->cmd = CtrlSocket(r, r->cmd_path, "cmd");
    if (r->ev < 0)
        r->ev = CtrlSocket(r, r->ev_path, "ev");
    if (r->cmd < 0 || r->ev < 0)
        return;
    struct timeval tv = { 1, 0 };
    setsockopt(r->ev, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char ok[16];
    if (send(r->ev, "ATTACH", 6, 0) < 0 || recv(r->ev, ok, sizeof(ok), 0) < 2 || strncmp(ok, "OK", 2))
    {
        CloseCtrl(r);
        return;
    }
    r->log("3DS radio: talking to hostapd on %s", r->ifname);
}

void R3Start(radio3ds *r, uds_host *u)
{
    StopHostapd(r);
    r->u = u;
    r->tags_len = UdsBeaconTags(u, r->tags);
    char ssid[9], ve[1100], conf[300], logf[300], ctrl[256];
    UdsSsid(u, ssid);
    Hex(ve, r->tags, r->tags_len);
    snprintf(conf, sizeof(conf), "%s/hostapd-3ds.conf", r->run_dir);
    snprintf(logf, sizeof(logf), "%s/hostapd-3ds.log", r->run_dir);
    snprintf(ctrl, sizeof(ctrl), "%s/hostapd3ds", r->run_dir);
    FILE *f = fopen(conf, "w");
    if (!f)
    {
        r->log("3DS radio: %s: %s", conf, strerror(errno));
        return;
    }
    // A 3DS network: the SSID (sent as zeros in beacons) is the network ID
    // in hex, 1 and 2 Mbit/s basic rates, open authentication, and the
    // Privacy bit set (our hostapd patch) though there is no WPA
    fprintf(f,
            "# Written by sionlinkd for the 3DS network (host/radio3ds.c)\n"
            "interface=%s\ndriver=nl80211\nctrl_interface=%s\n"
            "ssid=%s\nignore_broadcast_ssid=2\n"
            "hw_mode=g\nchannel=%d\nbeacon_int=100\ndtim_period=1\n"
            "basic_rates=10 20\nauth_algs=1\nwmm_enabled=0\nmax_num_sta=15\n"
            "force_privacy=1\nap_max_inactivity=600\nskip_inactivity_poll=1\ndisassoc_low_ack=0\n"
            "vendor_elements=%s\n",
            r->ifname, ctrl, ssid, r->channel, ve);
    fclose(f);

    pid_t pid = fork();
    if (pid == 0)
    {
        int fd = open(logf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        execl(r->hostapd, r->hostapd, "-t", conf, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
    {
        r->log("3DS radio: starting hostapd: %s", strerror(errno));
        return;
    }
    r->hostapd_pid = pid;
    r->retry_at = Ms() + 200;
    r->log("3DS radio: hostapd on %s, channel %d, network %s (log %s)", r->ifname, r->channel, ssid, logf);
}

// The beacon's elements, when the node list changed
static void UpdateBeacon(radio3ds *r)
{
    uint8_t tags[512];
    int len = UdsBeaconTags(r->u, tags);
    if (len == r->tags_len && !memcmp(tags, r->tags, len))
        return;
    if (!r->enabled)
        return; // the new elements go in once hostapd is up
    char hex[1100];
    Hex(hex, tags, len);
    const char *a = Command(r, "SET vendor_elements %s", hex);
    const char *b = a && !strncmp(a, "OK", 2) ? Command(r, "UPDATE_BEACON") : a;
    if (!b || strncmp(b, "OK", 2))
    {
        r->log("3DS radio: updating the beacon failed (%s)", b ? b : "no answer");
        return;
    }
    memcpy(r->tags, tags, len);
    r->tags_len = len;
}

static bool ParseMac(const char *s, uint8_t m[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        m[i] = v[i];
    return true;
}

static void Event(radio3ds *r, const char *e)
{
    if (*e == '<') // the level
    {
        const char *p = strchr(e, '>');
        if (p)
            e = p + 1;
    }
    uint8_t m[6];
    if (!strncmp(e, "AP-ENABLED", 10))
    {
        r->enabled = true;
        r->tags_len = -1; // send them again, in case they changed meanwhile
        InstallKey(r, NULL);
        UpdateBeacon(r);
    }
    else if (!strncmp(e, "AP-DISABLED", 11))
        r->enabled = false;
    else if (!strncmp(e, "AP-STA-CONNECTED ", 17) && ParseMac(e + 17, m))
    {
        InstallKey(r, m);
        if (r->u)
            UdsAssociated(r->u, m);
    }
    else if (!strncmp(e, "AP-STA-DISCONNECTED ", 20) && ParseMac(e + 20, m))
    {
        if (r->u)
            UdsLeft(r->u, m);
    }
}

// Data
// ====

void R3Tx(void *ctx, int kind, const uint8_t dst[6], const uint8_t *body, int len)
{
    radio3ds *r = ctx;
    switch (kind)
    {
    case UDS_BEACON:
        UpdateBeacon(r); // hostapd beacons; only the elements are ours
        break;
    case UDS_DEAUTH:
        Command(r, "DEAUTHENTICATE %s", Mac(dst));
        break;
    case UDS_DATA:
    {
        // [SNAP: AA AA 03 00 00 00, EtherType] payload -> Ethernet
        if (!r->enabled || len < 8 || len > 2000)
            return;
        uint8_t f[14 + 2000];
        memcpy(f, dst, 6);
        memcpy(f + 6, r->mac, 6);
        memcpy(f + 12, body + 6, 2);
        memcpy(f + 14, body + 8, len - 8);
        struct sockaddr_ll a = { .sll_family = AF_PACKET, .sll_ifindex = r->ifindex, .sll_halen = 6 };
        memcpy(a.sll_addr, dst, 6);
        for (int tries = 0;; tries++)
        {
            if (sendto(r->pkt, f, 14 + len - 8, 0, (struct sockaddr *)&a, sizeof(a)) >= 0)
                break;
            if ((errno == ENOBUFS || errno == EAGAIN) && tries < 20)
            {
                usleep(1000);
                continue;
            }
            r->log("3DS radio: send to %s: %s", Mac(dst), strerror(errno));
            break;
        }
        break;
    }
    default:
        break; // authentication and association are hostapd's
    }
}

static void Receive(radio3ds *r)
{
    uint8_t f[2048], body[2048];
    struct sockaddr_ll from;
    socklen_t fl = sizeof(from);
    int n;
    while ((n = recvfrom(r->pkt, f, sizeof(f), MSG_DONTWAIT, (struct sockaddr *)&from, &fl)) >= 0)
    {
        fl = sizeof(from);
        if (n < 14 || from.sll_pkttype == PACKET_OUTGOING || !r->u)
            continue;
        uint16_t type = (f[12] << 8) | f[13];
        if (type != UDS_ETH_DATA && type != UDS_ETH_EAPOL)
            continue;
        static const uint8_t llc[6] = { 0xAA, 0xAA, 0x03, 0, 0, 0 };
        memcpy(body, llc, 6);
        memcpy(body + 6, f + 12, 2);
        memcpy(body + 8, f + 14, n - 14);
        UdsReceive(r->u, UDS_DATA, f + 6, f, body, n - 14 + 8);
    }
}

// Open, poll, close
// =================

radio3ds *R3Open(const char *ifname, const char *hostapd, const char *run_dir,
                 int channel, int group_key_idx, void (*log)(const char *fmt, ...))
{
    radio3ds *r = calloc(1, sizeof(*r));
    snprintf(r->ifname, sizeof(r->ifname), "%s", ifname);
    snprintf(r->hostapd, sizeof(r->hostapd), "%s", hostapd);
    snprintf(r->run_dir, sizeof(r->run_dir), "%s", run_dir);
    r->channel = channel;
    r->group_key_idx = group_key_idx & 3;
    r->log = log;
    r->ev = r->cmd = r->nl = -1;
    mkdir(run_dir, 0755);

    r->pkt = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
    struct ifreq ifr = { 0 };
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (r->pkt < 0 || ioctl(r->pkt, SIOCGIFINDEX, &ifr) < 0)
    {
        log("3DS radio: %s: %s", ifname, strerror(errno));
        R3Close(r);
        return NULL;
    }
    r->ifindex = ifr.ifr_ifindex;
    if (ioctl(r->pkt, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(r->mac, ifr.ifr_hwaddr.sa_data, 6);
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL),
                               .sll_ifindex = r->ifindex };
    if (bind(r->pkt, (struct sockaddr *)&sll, sizeof(sll)) < 0)
    {
        log("3DS radio: bind %s: %s", ifname, strerror(errno));
        R3Close(r);
        return NULL;
    }
    int buf_sz = 1024 * 1024;
    setsockopt(r->pkt, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
    setsockopt(r->pkt, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));
    if (!NlOpen(r))
    {
        log("3DS radio: nl80211 isn't available: %s", strerror(errno));
        R3Close(r);
        return NULL;
    }
    log("3DS radio: %s (%s)", ifname, Mac(r->mac));
    return r;
}

void R3Close(radio3ds *r)
{
    if (!r)
        return;
    StopHostapd(r);
    if (r->pkt >= 0)
        close(r->pkt);
    if (r->nl >= 0)
        close(r->nl);
    free(r);
}

const uint8_t *R3Mac(radio3ds *r)
{
    return r->mac;
}

int R3Fds(radio3ds *r, int fds[2])
{
    fds[0] = r->pkt;
    fds[1] = r->ev;
    return 2;
}

void R3Poll(radio3ds *r)
{
    if (r->hostapd_pid > 0)
    {
        int status;
        if (waitpid(r->hostapd_pid, &status, WNOHANG) == r->hostapd_pid)
        {
            r->log("3DS radio: hostapd stopped (status %d); see %s/hostapd-3ds.log", status, r->run_dir);
            r->hostapd_pid = 0;
            r->enabled = false;
            CloseCtrl(r);
        }
        else if (r->ev < 0 && Ms() >= r->retry_at)
        {
            Attach(r);
            r->retry_at = Ms() + 200;
            if (r->cmd >= 0)
            {
                // Already up before we attached?
                const char *s = Command(r, "STATUS");
                if (s && !strcmp(s, "state=ENABLED"))
                    Event(r, "AP-ENABLED");
            }
        }
    }
    if (r->ev >= 0)
    {
        char e[1024];
        int n;
        while ((n = recv(r->ev, e, sizeof(e) - 1, MSG_DONTWAIT)) > 0)
        {
            e[n] = 0;
            char *nl = strchr(e, '\n');
            if (nl)
                *nl = 0;
            Event(r, e);
        }
    }
    Receive(r);
}
