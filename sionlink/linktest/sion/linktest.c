// Link Test: a game on the Sion with DS players over Sion Link.
//
// It registers with sionlinkd (common/slgame.h), which sends its DS client
// (linktest/ds) to every DS that links with the Sion Link app. Each player
// moves a square around the Sion's screen with the D-pad or the touch screen.
// The game runs at 60 frames a second: every frame it takes in each DS's
// input and sends one snapshot of the field to all of them, and both sides
// measure what arrives (linktest/ltproto.h). Once a second it prints a line
// per player, so a run over SSH is a log of how steady the link was.
//
// The screen is drawn with plain KMS dumb buffers at 720x480 (the Sion's TV
// output), so nothing else may be holding the display: "sion-dslink
// linktest" stops the dashboard first. --headless runs without a screen.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "font8x16.h"
#include "ltmeter.h"
#include "ltproto.h"
#include "slgame.h"

#define W LT_FIELD_W
#define H LT_FIELD_H
#define SPEED 4 // pixels a frame with the D-pad

// libnds key bits
#define KEY_RIGHT 0x0010
#define KEY_LEFT  0x0020
#define KEY_UP    0x0040
#define KEY_DOWN  0x0080

static volatile sig_atomic_t quit;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000);
}

static void logf_(const char *fmt, ...)
{
    time_t t = time(NULL);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
    printf("%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

// Players
// =======

typedef struct {
    bool here;
    char name[21];
    int x, y;
    uint16_t keys;
    bool touching;
    uint32_t echo_stamp, echo_at; // last input's DS stamp, and when it arrived
    lt_meter in;                  // their inputs, as the Sion sees them
    lt_stats ds_seen;             // our snapshots, as the DS saw them
    uint32_t joined;
    uint64_t rtt_sum, rtt_n;      // the DS's per-second averages, for the summary
} player;

static player players[LT_MAX_PLAYERS + 1]; // by player number, 1-15

static void join(int id, const char *name)
{
    if (id < 1 || id > LT_MAX_PLAYERS)
        return;
    player *p = &players[id];
    memset(p, 0, sizeof(*p));
    p->here = true;
    snprintf(p->name, sizeof(p->name), "%.20s", name);
    p->x = 120 + (id - 1) % 5 * 120;
    p->y = 160 + (id - 1) / 5 * 80;
    p->joined = now_ms();
    logf_("player %d (%s) joined", id, p->name);
}

static void summary(int id)
{
    player *p = &players[id];
    uint32_t secs = (now_ms() - p->joined) / 1000;
    uint64_t all = p->in.total_got + p->in.total_lost;
    logf_("player %d (%s), %u s: Sion got %llu of %llu inputs (%.2f%% lost), worst gap %u ms;"
          " DS round trip avg %llu ms",
          id, p->name, secs, (unsigned long long)p->in.total_got, (unsigned long long)all,
          all ? 100.0 * p->in.total_lost / all : 0.0, p->in.total_worst,
          p->rtt_n ? (unsigned long long)(p->rtt_sum / p->rtt_n) : 0ull);
}

static void leave(int id)
{
    if (id < 1 || id > LT_MAX_PLAYERS || !players[id].here)
        return;
    logf_("player %d (%s) left", id, players[id].name);
    summary(id);
    players[id].here = false;
}

static void input(int id, const uint8_t *b, int len, uint32_t now)
{
    if (id < 1 || id > LT_MAX_PLAYERS || !players[id].here || len < (int)sizeof(lt_input))
        return;
    lt_input in;
    memcpy(&in, b, sizeof(in));
    player *p = &players[id];
    if (!lt_meter_rx(&p->in, in.seq, now))
        return;
    p->keys = in.keys;
    p->touching = in.touching;
    if (in.touching)
    {
        p->x = in.touch_x * W / 256;
        p->y = in.touch_y * H / 192;
    }
    p->echo_stamp = in.stamp;
    p->echo_at = now;
    p->ds_seen = in.seen;
}

static void step(void)
{
    for (int id = 1; id <= LT_MAX_PLAYERS; id++)
    {
        player *p = &players[id];
        if (!p->here || p->touching)
            continue;
        if (p->keys & KEY_LEFT)
            p->x -= SPEED;
        if (p->keys & KEY_RIGHT)
            p->x += SPEED;
        if (p->keys & KEY_UP)
            p->y -= SPEED;
        if (p->keys & KEY_DOWN)
            p->y += SPEED;
        if (p->x < 0)
            p->x = 0;
        if (p->x > W - 1)
            p->x = W - 1;
        if (p->y < 0)
            p->y = 0;
        if (p->y > H - 1)
            p->y = H - 1;
    }
}

// sionlinkd
// =========

static int sock = -1;
static const char *sock_path = SLGAME_SOCKET;
static char client_path[256];

static bool link_open(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
    {
        if (fd >= 0)
            close(fd);
        return false;
    }
    uint8_t m[5 + sizeof(client_path)];
    uint32_t app = LT_APP_ID;
    m[0] = SLGAME_REGISTER;
    memcpy(m + 1, &app, 4);
    int n = strlen(client_path);
    memcpy(m + 5, client_path, n);
    if (send(fd, m, 5 + n, MSG_NOSIGNAL) < 0)
    {
        close(fd);
        return false;
    }
    sock = fd;
    logf_("registered with sionlinkd; DSes that link get %s", client_path);
    return true;
}

static void link_close(void)
{
    if (sock >= 0)
        close(sock);
    sock = -1;
    for (int id = 1; id <= LT_MAX_PLAYERS; id++)
        leave(id);
}

static void link_send(int to, const void *body, int len)
{
    if (sock < 0)
        return;
    uint8_t m[2 + 1024];
    m[0] = SLGAME_SEND;
    m[1] = to;
    memcpy(m + 2, body, len);
    if (send(sock, m, 2 + len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN)
    {
        logf_("lost sionlinkd: %s", strerror(errno));
        link_close();
    }
}

static void link_poll(uint32_t now)
{
    while (sock >= 0)
    {
        uint8_t m[SLGAME_MAX + 64];
        ssize_t n = recv(sock, m, sizeof(m), MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0)
        {
            logf_("sionlinkd went away");
            link_close();
            return;
        }
        if (n < 2)
            continue;
        switch (m[0])
        {
        case SLGAME_JOIN:
        {
            char name[21] = { 0 };
            memcpy(name, m + 2, n - 2 < 20 ? n - 2 : 20);
            join(m[1], name);
            break;
        }
        case SLGAME_LEAVE:
            leave(m[1]);
            break;
        case SLGAME_PACKET:
            if (n >= 3 && m[2] == LT_INPUT)
                input(m[1], m + 2, n - 2, now);
            break;
        case SLGAME_NOTE:
            logf_("sionlinkd: %.*s", (int)n - 2, (const char *)m + 2);
            break;
        }
    }
}

static void send_snapshot(uint32_t now)
{
    static uint32_t seq;
    uint8_t buf[sizeof(lt_snap) + LT_MAX_PLAYERS * sizeof(lt_player)];
    lt_snap s = { .kind = LT_SNAP, .seq = ++seq, .sion_ms = now };
    lt_player *out = (lt_player *)(buf + sizeof(lt_snap));
    for (int id = 1; id <= LT_MAX_PLAYERS; id++)
    {
        player *p = &players[id];
        if (!p->here)
            continue;
        lt_player *o = &out[s.count++];
        memset(o, 0, sizeof(*o));
        o->id = id;
        o->flags = p->touching;
        o->x = p->x;
        o->y = p->y;
        o->echo_stamp = p->echo_stamp;
        o->echo_hold = p->echo_stamp ? now - p->echo_at : 0;
        o->seen = p->in.done;
    }
    if (!s.count)
        return;
    memcpy(buf, &s, sizeof(s));
    link_send(SLGAME_ALL, buf, sizeof(lt_snap) + s.count * sizeof(lt_player));
}

static void send_names(void)
{
    lt_names n;
    memset(&n, 0, sizeof(n));
    n.kind = LT_NAMES;
    for (int id = 1; id <= LT_MAX_PLAYERS; id++)
        if (players[id].here)
        {
            n.p[n.count].id = id;
            memcpy(n.p[n.count].name, players[id].name, 10);
            n.count++;
        }
    if (n.count)
        link_send(SLGAME_ALL, &n, 2 + n.count * sizeof(n.p[0]));
}

// Drawing (720x480, 0xXXRRGGBB)
// =============================

static uint32_t canvas[W * H];

static void rect(int x, int y, int w, int h, uint32_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
            canvas[j * W + i] = c;
}

static void shade(int x, int y, int w, int h)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
            canvas[j * W + i] = (canvas[j * W + i] >> 2) & 0x3F3F3F;
}

static int text(int x, int y, uint32_t c, int scale, const char *fmt, ...)
{
    char s[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);
    for (const char *p = s; *p; p++, x += 8 * scale)
    {
        int ch = (unsigned char)*p;
        if (ch < 32 || ch > 126)
            ch = '?';
        const unsigned char *g = font8x16[ch - 32];
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 8; i++)
                if (g[j] & (0x80 >> i))
                    rect(x + i * scale, y + j * scale, scale, scale, c);
    }
    return x;
}

static uint32_t health(const lt_stats *s)
{
    if (s->got >= 58 && s->worst_gap <= 50)
        return 0x60E070; // steady
    if (s->got >= 50 && s->worst_gap <= 150)
        return 0xF0C030; // a few drops
    return 0xF05050;
}

static void draw(bool linked)
{
    for (int i = 0; i < W * H; i++)
        canvas[i] = 0x0A1428;
    for (int x = 0; x < W; x += 60)
        rect(x, 0, 1, H, 0x16243C);
    for (int y = 0; y < H; y += 60)
        rect(0, y, W, 1, 0x16243C);

    int n = 0;
    for (int id = 1; id <= LT_MAX_PLAYERS; id++)
    {
        player *p = &players[id];
        if (!p->here)
            continue;
        n++;
        uint32_t c = lt_colors[id];
        rect(p->x - 14, p->y - 14, 28, 28, 0xFFFFFF);
        rect(p->x - 12, p->y - 12, 24, 24, c);
        text(p->x - (int)strlen(p->name) * 4, p->y - 34, c, 1, "%s", p->name);
    }

    shade(0, 0, W, 40);
    text(16, 4, 0xFFFFFF, 2, "LINK TEST");
    if (!linked)
        text(190, 12, 0xF0C030, 1, "Waiting for sionlinkd (sion-dslink start)...");
    else if (!n)
        text(190, 12, 0xC0C8D8, 1, "Link a DS with the Sion Link app to play.");
    else
        text(190, 12, 0xC0C8D8, 1, "D-pad or touch moves your square. %d linked.", n);

    if (!n)
        return;
    int y = H - 24 - n * 18;
    shade(0, y - 8, W, H - y + 8);
    text(16, y, 0x8090A8, 1, "PLAYER        DS->SION  LOST  GAP    SION->DS  LOST  GAP   ROUND TRIP");
    y += 18;
    for (int id = 1; id <= LT_MAX_PLAYERS; id++)
    {
        player *p = &players[id];
        if (!p->here)
            continue;
        const lt_stats *a = &p->in.done, *b = &p->ds_seen;
        rect(16, y + 3, 10, 10, lt_colors[id]);
        text(32, y, 0xFFFFFF, 1, "%-10.10s", p->name);
        text(128, y, health(a), 1, "%5u/s %5u %4ums", a->got, a->lost, a->worst_gap);
        text(352, y, health(b), 1, "%5u/s %5u %4ums", b->got, b->lost, b->worst_gap);
        text(560, y, 0xFFFFFF, 1, "%3u ms (%u)", b->rtt_avg, b->rtt_max);
        y += 18;
    }
}

// KMS
// ===

static struct {
    int fd;
    uint32_t crtc, conn, fb[2], handle[2], pitch, mw, mh;
    uint8_t *map[2];
    size_t size;
    int back;
    bool flip_pending;
    struct drm_mode_crtc saved;
} kms = { .fd = -1 };

static bool kms_open(void)
{
    char path[32];
    for (int i = 0; i < 4 && kms.fd < 0; i++)
    {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        uint64_t dumb = 0;
        struct drm_get_cap cap = { .capability = DRM_CAP_DUMB_BUFFER };
        if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0)
            dumb = cap.value;
        if (dumb)
            kms.fd = fd;
        else
            close(fd);
    }
    if (kms.fd < 0)
    {
        logf_("no KMS display");
        return false;
    }

    struct drm_mode_card_res res = { 0 };
    if (ioctl(kms.fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return false;
    uint32_t crtcs[res.count_crtcs ? res.count_crtcs : 1];
    uint32_t conns[res.count_connectors ? res.count_connectors : 1];
    uint32_t encs[res.count_encoders ? res.count_encoders : 1];
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.connector_id_ptr = (uintptr_t)conns;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.count_fbs = 0;
    if (ioctl(kms.fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return false;

    struct drm_mode_modeinfo mode = { 0 };
    for (uint32_t i = 0; i < res.count_connectors && !kms.conn; i++)
    {
        struct drm_mode_get_connector c = { .connector_id = conns[i] };
        if (ioctl(kms.fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || c.connection != 1 || !c.count_modes)
            continue;
        struct drm_mode_modeinfo modes[c.count_modes];
        uint32_t encids[c.count_encoders ? c.count_encoders : 1];
        uint32_t nmodes = c.count_modes;
        c.modes_ptr = (uintptr_t)modes;
        c.encoders_ptr = (uintptr_t)encids;
        c.count_props = 0;
        if (ioctl(kms.fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || c.count_modes > nmodes)
            continue;
        // 720x480 (the Sion's TV mode), else the preferred one
        int pick = 0;
        for (uint32_t m = 0; m < c.count_modes; m++)
            if (modes[m].type & DRM_MODE_TYPE_PREFERRED)
                pick = m;
        for (uint32_t m = 0; m < c.count_modes; m++)
            if (modes[m].hdisplay == W && modes[m].vdisplay == H)
            {
                pick = m;
                break;
            }
        mode = modes[pick];
        // A CRTC this connector's encoder can drive
        for (uint32_t e = 0; e < c.count_encoders && !kms.crtc; e++)
        {
            struct drm_mode_get_encoder enc = { .encoder_id = encids[e] };
            if (ioctl(kms.fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
                continue;
            if (enc.crtc_id)
                kms.crtc = enc.crtc_id;
            else
                for (uint32_t k = 0; k < res.count_crtcs; k++)
                    if (enc.possible_crtcs & (1u << k))
                    {
                        kms.crtc = crtcs[k];
                        break;
                    }
        }
        if (kms.crtc)
            kms.conn = conns[i];
    }
    if (!kms.conn)
    {
        logf_("no connected display");
        return false;
    }
    kms.mw = mode.hdisplay;
    kms.mh = mode.vdisplay;

    for (int b = 0; b < 2; b++)
    {
        struct drm_mode_create_dumb cd = { .width = kms.mw, .height = kms.mh, .bpp = 32 };
        if (ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
            return false;
        kms.handle[b] = cd.handle;
        kms.pitch = cd.pitch;
        kms.size = cd.size;
        struct drm_mode_fb_cmd fb = { .width = kms.mw, .height = kms.mh, .pitch = cd.pitch,
                                      .bpp = 32, .depth = 24, .handle = cd.handle };
        if (ioctl(kms.fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0)
            return false;
        kms.fb[b] = fb.fb_id;
        struct drm_mode_map_dumb md = { .handle = cd.handle };
        if (ioctl(kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
            return false;
        kms.map[b] = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, kms.fd, md.offset);
        if (kms.map[b] == MAP_FAILED)
            return false;
        memset(kms.map[b], 0, cd.size);
    }

    kms.saved.crtc_id = kms.crtc;
    ioctl(kms.fd, DRM_IOCTL_MODE_GETCRTC, &kms.saved);
    struct drm_mode_crtc set = { .crtc_id = kms.crtc, .fb_id = kms.fb[0],
                                 .set_connectors_ptr = (uintptr_t)&kms.conn,
                                 .count_connectors = 1, .mode = mode, .mode_valid = 1 };
    if (ioctl(kms.fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0)
    {
        logf_("can't set the display mode: %s (is the dashboard still running?)", strerror(errno));
        return false;
    }
    kms.back = 1;
    logf_("display %ux%u on %s", kms.mw, kms.mh, path);
    return true;
}

// Waits for the last flip; returns false if none came in time.
static bool kms_wait(int ms)
{
    while (kms.flip_pending)
    {
        struct pollfd p = { kms.fd, POLLIN, 0 };
        if (poll(&p, 1, ms) <= 0)
            return false;
        char buf[256];
        ssize_t n = read(kms.fd, buf, sizeof(buf));
        for (ssize_t o = 0; o + (ssize_t)sizeof(struct drm_event) <= n;)
        {
            struct drm_event *e = (struct drm_event *)(buf + o);
            if (e->type == DRM_EVENT_FLIP_COMPLETE)
                kms.flip_pending = false;
            o += e->length;
        }
    }
    return true;
}

// Shows the canvas, centered (and cut to fit, on a smaller mode).
static void kms_show(void)
{
    uint8_t *dst = kms.map[kms.back];
    int ox = ((int)kms.mw - W) / 2, oy = ((int)kms.mh - H) / 2;
    for (uint32_t y = 0; y < kms.mh; y++)
    {
        uint32_t *row = (uint32_t *)(dst + y * kms.pitch);
        int sy = (int)y - oy;
        for (uint32_t x = 0; x < kms.mw; x++)
        {
            int sx = (int)x - ox;
            row[x] = (sx >= 0 && sx < W && sy >= 0 && sy < H) ? canvas[sy * W + sx] : 0;
        }
    }
    struct drm_mode_crtc_page_flip f = { .crtc_id = kms.crtc, .fb_id = kms.fb[kms.back],
                                         .flags = DRM_MODE_PAGE_FLIP_EVENT };
    if (ioctl(kms.fd, DRM_IOCTL_MODE_PAGE_FLIP, &f) == 0)
    {
        kms.flip_pending = true;
        kms.back ^= 1;
    }
}

static void kms_close(void)
{
    if (kms.fd < 0)
        return;
    kms_wait(100);
    if (kms.saved.fb_id)
    {
        kms.saved.set_connectors_ptr = (uintptr_t)&kms.conn;
        kms.saved.count_connectors = 1;
        ioctl(kms.fd, DRM_IOCTL_MODE_SETCRTC, &kms.saved);
    }
    close(kms.fd);
    kms.fd = -1;
}

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++)
    {
        uint8_t px[3] = { canvas[i] >> 16, canvas[i] >> 8, canvas[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

// Main
// ====

static void on_signal(int sig)
{
    quit = 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: linktest [--client FILE.nds] [--sock PATH] [--headless]\n"
                    "                [--shot FILE.ppm] [--seconds N]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    bool headless = false;
    const char *shot = NULL;
    int seconds = 0;

    // The client next to this program, by default
    snprintf(client_path, sizeof(client_path), "linktest.nds");
    char self[200];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl > 0)
    {
        self[sl] = 0;
        char *slash = strrchr(self, '/');
        if (slash)
        {
            *slash = 0;
            snprintf(client_path, sizeof(client_path), "%s/linktest.nds", self);
        }
    }

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--client") && i + 1 < argc)
        {
            const char *c = argv[++i];
            if (c[0] == '/')
                snprintf(client_path, sizeof(client_path), "%s", c);
            else
            {
                char cwd[128];
                snprintf(client_path, sizeof(client_path), "%s/%s",
                         getcwd(cwd, sizeof(cwd)) ? cwd : ".", c);
            }
        }
        else if (!strcmp(argv[i], "--sock") && i + 1 < argc)
            sock_path = argv[++i];
        else if (!strcmp(argv[i], "--headless"))
            headless = true;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            shot = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else
            usage();
    }
    if (access(client_path, R_OK) < 0)
    {
        fprintf(stderr, "linktest: no DS client at %s\n", client_path);
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!headless && !kms_open())
    {
        kms_close();
        fprintf(stderr, "linktest: no display; run with --headless to play without one\n");
        return 1;
    }

    uint32_t start = now_ms(), last_try = 0, last_names = 0, last_log = start;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!quit)
    {
        uint32_t now = now_ms();
        if (seconds && now - start >= (uint32_t)seconds * 1000)
            break;
        if (sock < 0 && now - last_try >= 1000)
        {
            last_try = now;
            link_open();
        }
        link_poll(now);
        step();
        send_snapshot(now);
        if (now - last_names >= 1000)
        {
            send_names();
            last_names = now;
        }

        for (int id = 1; id <= LT_MAX_PLAYERS; id++)
        {
            player *p = &players[id];
            if (!p->here)
                continue;
            if (lt_meter_tick(&p->in, now) && p->ds_seen.rtt_avg)
            {
                p->rtt_sum += p->ds_seen.rtt_avg;
                p->rtt_n++;
            }
        }
        if (now - last_log >= 1000)
        {
            last_log += 1000;
            for (int id = 1; id <= LT_MAX_PLAYERS; id++)
            {
                player *p = &players[id];
                if (!p->here)
                    continue;
                const lt_stats *a = &p->in.done, *b = &p->ds_seen;
                logf_("p%d %-10.10s DS->Sion %2u/s lost %u gap %3u ms | Sion->DS %2u/s lost %u"
                      " gap %3u ms | rtt %u ms (max %u)",
                      id, p->name, a->got, a->lost, a->worst_gap, b->got, b->lost,
                      b->worst_gap, b->rtt_avg, b->rtt_max);
            }
        }

        if (!headless || shot)
            draw(sock >= 0);
        if (!headless)
        {
            // The display's vblank paces the game; the timer covers a
            // display that doesn't send flip events.
            kms_wait(40);
            kms_show();
        }
        next.tv_nsec += 16666667;
        if (next.tv_nsec >= 1000000000)
        {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        if (headless)
        {
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
        else
        {
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            next = t; // vblank-paced: no catching up
        }
    }

    if (shot)
        save_ppm(shot);
    for (int id = 1; id <= LT_MAX_PLAYERS; id++)
        if (players[id].here)
            summary(id);
    kms_close();
    if (sock >= 0)
        close(sock);
    return 0;
}
