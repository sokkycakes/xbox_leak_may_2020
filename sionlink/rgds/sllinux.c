// Sion Link on Linux handhelds (see sllinux.h).

#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
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

#include "sllinux.h"

#define RUN_DIR  "/run/sionlink-client"
#define CTRL_DIR RUN_DIR "/ctrl"

static char ifname[IFNAMSIZ] = "";
static char owner[20] = "";
static bool (*cancel_cb)(void);
static bool direct;
static uint8_t sion_mac[6], my_mac[6];
static char sion_ssid[33];
static int sion_freq;
static int heard;
static int pkt_sock = -1, ifindex;
static pid_t wpa_pid;

uint32_t SlMillis(void)
{
    static struct timespec t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!t0.tv_sec && !t0.tv_nsec)
        t0 = t;
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

void SlSetInterface(const char *name) { snprintf(ifname, sizeof(ifname), "%s", name); }
void SlSetName(const char *name) { snprintf(owner, sizeof(owner), "%s", name); }
void SlSetCancel(bool (*cancel)(void)) { cancel_cb = cancel; }
int SlHeard(void) { return heard; }

void SlSetDirectPeer(const uint8_t mac[6])
{
    direct = true;
    memcpy(sion_mac, mac, 6);
}

void SlNetwork(uint8_t bssid[6], char ssid[33], int *freq)
{
    memcpy(bssid, sion_mac, 6);
    memcpy(ssid, sion_ssid, 33);
    *freq = sion_freq;
}

static void Say(void (*status)(const char *), const char *s)
{
    if (status)
        status(s);
}

static bool Cancelled(void) { return cancel_cb && cancel_cb(); }

static void Nap(int ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

bool SlInit(void)
{
    if (!ifname[0])
        SlSetInterface(getenv("SL_IFACE") ? getenv("SL_IFACE") : "wlan0");
    if (!owner[0])
    {
        const char *n = getenv("SL_NAME");
        char h[64] = "RG DS";
        if (!n && gethostname(h, sizeof(h)) == 0)
            h[sizeof(h) - 1] = 0;
        SlSetName(n ? n : h);
    }
    mkdir(RUN_DIR, 0700);
    pkt_sock = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (pkt_sock < 0)
    {
        fprintf(stderr, "sllinux: packet socket: %s\n", strerror(errno));
        return false;
    }
    struct ifreq ifr = { 0 };
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(pkt_sock, SIOCGIFINDEX, &ifr) < 0)
    {
        fprintf(stderr, "sllinux: %s: %s\n", ifname, strerror(errno));
        return false;
    }
    ifindex = ifr.ifr_ifindex;
    if (ioctl(pkt_sock, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(my_mac, ifr.ifr_hwaddr.sa_data, 6);
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL),
                               .sll_ifindex = ifindex };
    if (bind(pkt_sock, (struct sockaddr *)&sll, sizeof(sll)) < 0)
    {
        fprintf(stderr, "sllinux: bind %s: %s\n", ifname, strerror(errno));
        return false;
    }
    return true;
}

// wpa_supplicant control socket
// =============================

static int ctrl = -1;
static char ctrl_local[108];

static void CtrlClose(void)
{
    if (ctrl >= 0)
        close(ctrl);
    ctrl = -1;
    if (ctrl_local[0])
        unlink(ctrl_local);
}

static bool CtrlOpen(void)
{
    CtrlClose();
    ctrl = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ctrl < 0)
        return false;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(ctrl_local, sizeof(ctrl_local), RUN_DIR "/cli-%d", (int)getpid());
    unlink(ctrl_local);
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", ctrl_local);
    if (bind(ctrl, (struct sockaddr *)&a, sizeof(a)) < 0)
        return false;
    struct sockaddr_un r = { .sun_family = AF_UNIX };
    snprintf(r.sun_path, sizeof(r.sun_path), CTRL_DIR "/%s", ifname);
    return connect(ctrl, (struct sockaddr *)&r, sizeof(r)) == 0;
}

// Sends one command and returns its reply (unsolicited "<N>" events skipped)
static const char *Ctrl(const char *fmt, ...)
{
    static char reply[16384];
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    reply[0] = 0;
    if (ctrl < 0 || send(ctrl, cmd, strlen(cmd), 0) < 0)
        return reply;
    for (;;)
    {
        struct pollfd p = { ctrl, POLLIN, 0 };
        if (poll(&p, 1, 3000) <= 0)
            return reply;
        ssize_t n = recv(ctrl, reply, sizeof(reply) - 1, 0);
        if (n < 0)
            return "";
        reply[n] = 0;
        if (reply[0] != '<')
            return reply;
    }
}

static void WpaStop(void)
{
    if (direct)
        return;
    if (ctrl >= 0)
        Ctrl("TERMINATE");
    CtrlClose();
    if (wpa_pid > 0)
    {
        for (int i = 0; i < 20 && waitpid(wpa_pid, NULL, WNOHANG) == 0; i++)
            Nap(100);
        if (waitpid(wpa_pid, NULL, WNOHANG) == 0)
        {
            kill(wpa_pid, SIGKILL);
            waitpid(wpa_pid, NULL, 0);
        }
    }
    wpa_pid = 0;
}

static bool WpaStart(void)
{
    WpaStop();
    FILE *f = fopen(RUN_DIR "/wpa.conf", "w");
    if (!f)
        return false;
    // ap_scan 1, no networks yet: we pick the Sion from the scan ourselves
    fprintf(f, "ctrl_interface=%s\n", CTRL_DIR);
    fclose(f);
    const char *bin = getenv("SL_WPA_SUPPLICANT");
    if (!bin)
        bin = access("/usr/sbin/wpa_supplicant", X_OK) == 0 ? "/usr/sbin/wpa_supplicant" : "wpa_supplicant";
    wpa_pid = fork();
    if (wpa_pid == 0)
    {
        int log = open(RUN_DIR "/wpa.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log >= 0)
        {
            dup2(log, 1);
            dup2(log, 2);
        }
        execlp(bin, bin, "-i", ifname, "-D", "nl80211", "-c", RUN_DIR "/wpa.conf", (char *)NULL);
        _exit(127);
    }
    if (wpa_pid < 0)
        return false;
    for (int i = 0; i < 50; i++)
    {
        if (CtrlOpen() && !strncmp(Ctrl("PING"), "PONG", 4))
            return true;
        if (waitpid(wpa_pid, NULL, WNOHANG) != 0)
            break;
        Nap(100);
    }
    fprintf(stderr, "sllinux: wpa_supplicant (%s) didn't start\n", bin);
    wpa_pid = 0;
    return false;
}

static int Hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// "key=value" line from a BSS / STATUS reply
static bool Field(const char *reply, const char *key, char *out, int size)
{
    int kl = strlen(key);
    for (const char *p = reply; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
    {
        if (!strncmp(p, key, kl) && p[kl] == '=')
        {
            p += kl + 1;
            int n = strcspn(p, "\n");
            if (n >= size)
                n = size - 1;
            memcpy(out, p, n);
            out[n] = 0;
            return true;
        }
    }
    return false;
}

// Whether a beacon's elements (hex) carry Nintendo's element with our game
// ID, as DSWifi reads it (host/sionlinkd.c builds it)
static bool IsSion(const char *iehex)
{
    static uint8_t ie[2048];
    int n = 0;
    for (const char *p = iehex; Hex(p[0]) >= 0 && Hex(p[1]) >= 0 && n < (int)sizeof(ie); p += 2)
        ie[n++] = Hex(p[0]) << 4 | Hex(p[1]);
    for (int i = 0; i + 2 <= n && i + 2 + ie[i + 1] <= n; i += 2 + ie[i + 1])
    {
        const uint8_t *e = ie + i + 2;
        if (ie[i] == 0xDD && ie[i + 1] >= 16 && e[0] == 0x00 && e[1] == 0x09 && e[2] == 0xBF && e[3] == 0x00 &&
            (uint32_t)(e[12] << 24 | e[13] << 16 | e[14] << 8 | e[15]) == SL_GAME_ID)
            return true;
    }
    return false;
}

static bool ParseMac(const char *s, uint8_t mac[6])
{
    unsigned m[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        mac[i] = m[i];
    return true;
}

// Scans until a Sion turns up; false if cancelled
static bool FindSion(void (*status)(const char *))
{
    uint32_t last_scan = 0;
    bool first = true;
    for (;;)
    {
        if (Cancelled())
            return false;
        uint32_t now = SlMillis();
        if (first || now - last_scan > 4000)
        {
            Ctrl("SCAN");
            last_scan = now;
            first = false;
        }
        Nap(500);
        int n = 0;
        for (int i = 0; i < 64; i++)
        {
            const char *r = Ctrl("BSS %d", i); // a number is a place in the list
            char bssid[32], ie[4200], ssid[80], freq[16];
            if (!Field(r, "bssid", bssid, sizeof(bssid)))
                break;
            n++;
            if (Field(r, "ie", ie, sizeof(ie)) && IsSion(ie) && ParseMac(bssid, sion_mac))
            {
                if (!Field(r, "ssid", ssid, sizeof(ssid)))
                    ssid[0] = 0;
                snprintf(sion_ssid, sizeof(sion_ssid), "%s", ssid);
                sion_freq = Field(r, "freq", freq, sizeof(freq)) ? atoi(freq) : 0;
                heard = n;
                return true;
            }
        }
        if (n != heard)
        {
            heard = n;
            char text[96];
            snprintf(text, sizeof(text), "Looking for the Sion...\n\n%d networks heard", n);
            Say(status, text);
        }
    }
}

static void HexOf(const char *s, char *out)
{
    for (; *s; s++, out += 2)
        sprintf(out, "%02x", (unsigned char)*s);
    *out = 0;
}

static bool Associate(void (*status)(const char *))
{
    Ctrl("REMOVE_NETWORK all");
    const char *id = Ctrl("ADD_NETWORK");
    int net = atoi(id);
    char ssidhex[70];
    HexOf(sion_ssid, ssidhex);
    Ctrl("SET_NETWORK %d ssid %s", net, ssidhex);
    Ctrl("SET_NETWORK %d key_mgmt NONE", net);
    Ctrl("SET_NETWORK %d bssid %02x:%02x:%02x:%02x:%02x:%02x", net, sion_mac[0], sion_mac[1],
         sion_mac[2], sion_mac[3], sion_mac[4], sion_mac[5]);
    if (sion_freq)
        Ctrl("SET_NETWORK %d scan_freq %d", net, sion_freq);
    Ctrl("SELECT_NETWORK %d", net);
    Say(status, "Linking...");
    uint32_t t0 = SlMillis();
    while (SlMillis() - t0 < 15000)
    {
        if (Cancelled())
            return false;
        char st[32];
        if (Field(Ctrl("STATUS"), "wpa_state", st, sizeof(st)) && !strcmp(st, "COMPLETED"))
        {
            // Power save off: with it on, the Sion's broadcasts wait for the
            // next beacon (up to 100 ms) and so does everything we send
            Ctrl("SET ps 0");
            return true;
        }
        Nap(100);
    }
    return false;
}

// Packets
// =======

#define RXQ_MAX (sizeof(sl_hdr) + SL_MAX_BODY)
static uint8_t current[RXQ_MAX];
static uint32_t last_heard;
static int player;
static uint16_t tx_seq;

bool SlJoin(void (*status)(const char *text))
{
    player = 0;
    if (!direct)
    {
        Say(status, "Starting Wi-Fi...");
        for (;;)
        {
            if (Cancelled())
                return false;
            if (WpaStart())
                break;
            Say(status, "Couldn't start Wi-Fi.\nIs the handheld's own Wi-Fi off?");
            Nap(2000);
        }
        for (;;)
        {
            Say(status, "Looking for the Sion...");
            if (!FindSion(status))
                return false;
            if (Associate(status))
                break;
            Say(status, "The Sion didn't let us in. Trying again...");
            Nap(1000);
        }
    }
    // Drop whatever queued up before we joined
    uint8_t junk[2048];
    while (recv(pkt_sock, junk, sizeof(junk), MSG_DONTWAIT) >= 0)
        ;
    last_heard = SlMillis();
    return true;
}

bool SlNext(const sl_hdr **hdr, const uint8_t **body)
{
    uint8_t f[2048];
    struct sockaddr_ll from;
    for (;;)
    {
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(pkt_sock, f, sizeof(f), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n < 0)
            return false;
        if (from.sll_pkttype == PACKET_OUTGOING || n < ETH_HLEN + 6 + (int)sizeof(sl_hdr))
            continue;
        // From the Sion, 802.3 length field (no LLC/SNAP): [to MAC][sl_hdr]...
        uint16_t lt = f[12] << 8 | f[13];
        if (lt > 1500 || memcmp(f + 6, sion_mac, 6))
            continue;
        int plen = n - ETH_HLEN;
        if (lt < plen)
            plen = lt;
        const uint8_t *pl = f + ETH_HLEN;
        static const uint8_t all[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        if (memcmp(pl, my_mac, 6) && memcmp(pl, all, 6))
            continue;
        pl += 6;
        plen -= 6;
        const sl_hdr *h = (const sl_hdr *)pl;
        if (plen > (int)RXQ_MAX || h->magic[0] != SL_MAGIC0 || h->magic[1] != SL_MAGIC1 ||
            h->len > plen - (int)sizeof(sl_hdr))
            continue;
        memcpy(current, pl, plen);
        h = (const sl_hdr *)current;
        last_heard = SlMillis();
        if (h->type == SL_WELCOME && h->len >= sizeof(sl_welcome))
            player = ((const sl_welcome *)(current + sizeof(sl_hdr)))->client_id;
        *hdr = h;
        *body = current + sizeof(sl_hdr);
        return true;
    }
}

uint32_t SlQuietFor(uint32_t now_ms)
{
    int32_t d = (int32_t)(now_ms - last_heard);
    return d > 0 ? (uint32_t)d : 0;
}

void SlSend(int type, const void *body, int len)
{
    if (len > SL_MAX_BODY)
        return;
    // [dst][src][length] [client AID][pad][sl_hdr][body], as a DS sends it
    uint8_t f[ETH_HLEN + 2 + sizeof(sl_hdr) + SL_MAX_BODY + 16];
    memcpy(f, sion_mac, 6);
    memcpy(f + 6, my_mac, 6);
    int plen = 2 + sizeof(sl_hdr) + len;
    f[12] = plen >> 8;
    f[13] = plen & 0xFF;
    uint8_t *pl = f + ETH_HLEN;
    pl[0] = player; // sionlinkd goes by MAC; the DS puts its AID here
    pl[1] = 0;
    sl_hdr *h = (sl_hdr *)(pl + 2);
    h->magic[0] = SL_MAGIC0;
    h->magic[1] = SL_MAGIC1;
    h->version = SL_VERSION;
    h->type = type;
    h->seq = tx_seq++;
    h->len = len;
    if (len)
        memcpy(pl + 2 + sizeof(sl_hdr), body, len);
    int flen = ETH_HLEN + plen;
    if (flen < 60)
    {
        memset(f + flen, 0, 60 - flen);
        flen = 60;
    }
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_ifindex = ifindex, .sll_halen = 6 };
    memcpy(sll.sll_addr, sion_mac, 6);
    sendto(pkt_sock, f, flen, 0, (struct sockaddr *)&sll, sizeof(sll));
}

int SlKeepAlive(uint32_t app, uint32_t now)
{
    static uint32_t last_hello;
    static bool sent;
    if (!sent || now - last_hello > (player ? 3000u : 500u))
    {
        sl_hello hello = { 0 };
        memcpy(hello.name, owner, sizeof(hello.name) - 1);
        hello.app = app;
        SlSend(SL_HELLO, &hello, sizeof(hello));
        last_hello = now;
        sent = true;
    }
    return player;
}

void SlExit(void)
{
    if (pkt_sock >= 0 && player)
        SlSend(SL_BYE, NULL, 0);
    player = 0;
    Nap(50);
    if (ctrl >= 0)
        Ctrl("DISCONNECT");
    WpaStop();
    if (pkt_sock >= 0)
        close(pkt_sock);
    pkt_sock = -1;
}
