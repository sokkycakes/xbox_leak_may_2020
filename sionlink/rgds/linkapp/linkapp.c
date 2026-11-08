// Sion Link for the RG DS: the DS app (mirror/ds, sion-link-live.nds) as a native Linux
// program, with the same screens (mirror/ds/source/nitroui.c, built here on a small libnds
// shim) on the RG DS's two panels. Like the DS app it does its three things by itself:
//
//   Controller     while no DS game is mirrored, the RG DS's buttons and touch are a pad for
//                  the Sion: reports go to the Sion's second dspad (TCP 7791), which makes an
//                  Xbox 360 pad of them, as the first one does for a DS on the DSpico.
//   Mirror         once a DS title runs on the Sion (its mirrorrun answers on TCP 7790), the
//                  game takes both panels; when the title ends, or F is pressed, the app's
//                  home comes back. The picture still comes through the DS app run in the
//                  mirror's DS core (mirrorrun --view sdl), handed the connection made here.
//   Download Play  on the Sion's own DS network (as a DS joins it: sllinux.c), programs the
//                  Sion sends are taken in and started in that DS core.
//
// The Sion: on the home Wi-Fi when it answers at SION_HOST there, else on its DS network
// (Y switches between the two; Download Play needs the DS network). F quits.
//
//   sionlink-app [--mirrorrun PATH] [--nds PATH] [--host ADDR] [--dsnet] [--headless]
//
// Environment: SION_HOST (192.168.1.247), SL_NAME, SL_IFACE, SL_WIFI_DOWN / SL_WIFI_UP (the
// commands that take the handheld's own Wi-Fi down and back), SL_SCREENSHOT=FILE.BMP (the
// panels as shown, once a second, for tests).
#define _GNU_SOURCE
#include <SDL.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nds.h"
#include "nitroui.h"
#include "mirror_format.h"
#include "mirror_lz.h"
#include "sllinux.h"

u16 shimTop[256 * 256], shimBottom[256 * 256];
vu32 shimReg;
void uiPoll(void) {}

#define MIRROR_PORT 7790
#define PAD_PORT 7791

static const char* mirrorrun = "/tmp/sion-mirrorrun";
static const char* clientNds = "sion-link-live.nds";
static const char* fixedHost;
static bool headless, startOnDsNet;
static volatile sig_atomic_t quit;

static uint32_t NowMs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

static void Log(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    time_t t = time(NULL);
    char when[16];
    strftime(when, sizeof when, "%T", localtime(&t));
    fprintf(stderr, "%s linkapp: ", when);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
}

// ---------------------------------------------------------------- the panels

static SDL_Window* win;
static uint32_t W = 640, H = 960;   // the window: both panels, stacked
static uint32_t sw, sh, sx0, sy0[2];
static SDL_GameController* pads[4];
static bool touching;
static uint16_t tx, ty;

static void Scale(void)
{
    uint32_t hh = H / 2;
    sw = W, sh = W * 3 / 4;
    if (sh > hh) sh = hh, sw = hh * 4 / 3;
    sx0 = (W - sw) / 2;
    sy0[0] = (hh - sh) / 2;
    sy0[1] = hh + (hh - sh) / 2;
}

static bool OpenPanels(void)
{
    if (headless) return true;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0)
    {
        Log("SDL: %s", SDL_GetError());
        return false;
    }
    SDL_DisplayMode dm;
    if (getenv("SDL_BANANA_PANELS") && SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0)
        W = dm.w, H = dm.h;
    win = SDL_CreateWindow("Sion Link", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H,
                           getenv("SDL_BANANA_PANELS") ? SDL_WINDOW_FULLSCREEN : 0);
    if (!win)
    {
        Log("window: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    W = w, H = h;
    SDL_ShowCursor(SDL_DISABLE);
    Scale();
    return true;
}

// for the mirror and the DS core, which open the panels themselves
static void ClosePanels(void)
{
    if (headless) return;
    for (int i = 0; i < 4; i++)
        if (pads[i]) SDL_GameControllerClose(pads[i]), pads[i] = NULL;
    if (win) SDL_DestroyWindow(win);
    win = NULL;
    SDL_Quit();
}

static uint32_t Rgb(u16 c)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (r << 19 | (r >> 2) << 16) | (g << 11 | (g >> 2) << 8) | (b << 3 | b >> 2);
}

static void Compose(uint32_t* dst, uint32_t stride)
{
    for (uint32_t y = 0; y < H; y++) memset(dst + y * stride, 0, W * 4);
    const u16* src[2] = {shimTop, shimBottom};
    for (int s = 0; s < 2; s++)
        for (uint32_t y = 0; y < sh; y++)
        {
            uint32_t* row = dst + (sy0[s] + y) * stride + sx0;
            const u16* in = src[s] + (y * 192 / sh) * 256;
            for (uint32_t x = 0; x < sw; x++) row[x] = Rgb(in[x * 256 / sw]);
        }
}

static void Present(void)
{
    if (headless || !win) return;
    SDL_Surface* s = SDL_GetWindowSurface(win);
    if (!s) return;
    static uint32_t* buf;
    static size_t bufN;
    if (bufN != (size_t)W * H) free(buf), buf = malloc((size_t)W * H * 4), bufN = (size_t)W * H;
    Compose(buf, W);
    SDL_Surface* from = SDL_CreateRGBSurfaceWithFormatFrom(buf, W, H, 32, W * 4, SDL_PIXELFORMAT_ARGB8888);
    if (from) SDL_BlitSurface(from, NULL, s, NULL), SDL_FreeSurface(from);
    SDL_UpdateWindowSurface(win);
    static uint32_t shotAt;
    const char* shot = getenv("SL_SCREENSHOT");
    if (shot && NowMs() - shotAt >= 1000)
    {
        shotAt = NowMs();
        SDL_Surface* f = SDL_CreateRGBSurfaceWithFormatFrom(buf, W, H, 32, W * 4, SDL_PIXELFORMAT_ARGB8888);
        if (f) SDL_SaveBMP(f, shot), SDL_FreeSurface(f);
    }
}

// libnds key bits, as the DS app reports them
enum { K_A = 1, K_B = 2, K_SELECT = 4, K_START = 8, K_RIGHT = 16, K_LEFT = 32, K_UP = 64, K_DOWN = 128,
       K_R = 256, K_L = 512, K_X = 1024, K_Y = 2048, K_TOUCH = 4096 };

static bool OnBottom(float px, float py, uint16_t* x, uint16_t* y)
{
    if (px < sx0 || py < sy0[1] || px >= sx0 + sw || py >= sy0[1] + sh) return false;
    *x = (uint16_t)((px - sx0) * 256 / sw);
    *y = (uint16_t)((py - sy0[1]) * 192 / sh);
    return true;
}

// once a frame: the keys held (libnds bits)
static uint32_t Input(void)
{
    if (headless) return 0;
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        switch (e.type)
        {
        case SDL_QUIT: quit = 1; break;
        case SDL_CONTROLLERDEVICEADDED:
            for (int i = 0; i < 4; i++)
                if (!pads[i]) { pads[i] = SDL_GameControllerOpen(e.cdevice.which); break; }
            break;
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_ESCAPE) quit = 1;
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
            touching = OnBottom(e.tfinger.x * W, e.tfinger.y * H, &tx, &ty);
            break;
        case SDL_FINGERUP: touching = false; break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT)
                touching = OnBottom(e.button.x, e.button.y, &tx, &ty);
            break;
        case SDL_MOUSEMOTION:
            if (e.motion.which != SDL_TOUCH_MOUSEID && (e.motion.state & SDL_BUTTON_LMASK))
                touching = OnBottom(e.motion.x, e.motion.y, &tx, &ty);
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) touching = false;
            break;
        }
    }
    uint32_t k = touching ? K_TOUCH : 0;
    for (int i = 0; i < 4; i++)
    {
        SDL_GameController* c = pads[i];
        if (!c) continue;
#define B(btn) (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_##btn) != 0)
        if (B(GUIDE)) quit = 1;
        // by where they sit, as on a DS (SDL's names are an Xbox pad's)
        if (B(B)) k |= K_A;
        if (B(A)) k |= K_B;
        if (B(Y)) k |= K_X;
        if (B(X)) k |= K_Y;
        if (B(BACK)) k |= K_SELECT;
        if (B(START)) k |= K_START;
        if (B(LEFTSHOULDER)) k |= K_L;
        if (B(RIGHTSHOULDER)) k |= K_R;
        if (B(DPAD_UP)) k |= K_UP;
        if (B(DPAD_DOWN)) k |= K_DOWN;
        if (B(DPAD_LEFT)) k |= K_LEFT;
        if (B(DPAD_RIGHT)) k |= K_RIGHT;
#undef B
        int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (lx < -16000) k |= K_LEFT;
        if (lx > 16000) k |= K_RIGHT;
        if (ly < -16000) k |= K_UP;
        if (ly > 16000) k |= K_DOWN;
    }
    const uint8_t* kb = SDL_GetKeyboardState(NULL);
    if (kb[SDL_SCANCODE_X]) k |= K_A;
    if (kb[SDL_SCANCODE_Z]) k |= K_B;
    if (kb[SDL_SCANCODE_S]) k |= K_X;
    if (kb[SDL_SCANCODE_A]) k |= K_Y;
    if (kb[SDL_SCANCODE_Q]) k |= K_L;
    if (kb[SDL_SCANCODE_W]) k |= K_R;
    if (kb[SDL_SCANCODE_RETURN]) k |= K_START;
    if (kb[SDL_SCANCODE_RSHIFT]) k |= K_SELECT;
    if (kb[SDL_SCANCODE_UP]) k |= K_UP;
    if (kb[SDL_SCANCODE_DOWN]) k |= K_DOWN;
    if (kb[SDL_SCANCODE_LEFT]) k |= K_LEFT;
    if (kb[SDL_SCANCODE_RIGHT]) k |= K_RIGHT;
    return k;
}

// ---------------------------------------------------------------- TCP to the Sion

// host may be "[fe80::...%wlan0]" or a name; -1 if nothing answers within ms
static int TcpConnect(const char* host, int port, int ms)
{
    char h[96], p[8];
    snprintf(h, sizeof h, "%s", host);
    if (h[0] == '[')
    {
        memmove(h, h + 1, strlen(h));
        char* e = strchr(h, ']');
        if (e) *e = 0;
    }
    snprintf(p, sizeof p, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(h, p, &hints, &res)) return -1;
    int fd = -1;
    for (struct addrinfo* r = res; r && fd < 0; r = r->ai_next)
    {
        fd = socket(r->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) && errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        struct pollfd pf = {fd, POLLOUT, 0};
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&pf, 1, ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(fd); fd = -1; }
    }
    freeaddrinfo(res);
    if (fd >= 0)
    {
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    }
    return fd;
}

// The Sion's second dspad: reports out once a frame, its pad bundles in
static int padFd = -1;
static uint8_t padAcc[8192];
static size_t padN;
static bool padOn;
static char padText[2][64];
static uint32_t padHeardAt;

static void PadClose(void)
{
    if (padFd >= 0) close(padFd);
    padFd = -1;
    padN = 0;
    padOn = false;
}

// a bundle from dspad: MIRROR_FLAG_PAD, packed (mirror_lz.h), one MREC_PAD
static void PadBundle(const uint8_t* b, uint32_t n)
{
    mirror_hdr h;
    memcpy(&h, b, sizeof h);
    if (!(h.flags & MIRROR_FLAG_PAD)) return;
    static uint8_t raw[4096];
    const uint8_t* body = b + sizeof h;
    uint32_t bodyN = n - sizeof h;
    if (h.flags & MIRROR_FLAG_LZ)
    {
        uint32_t w[3];
        if (bodyN < sizeof w) return;
        memcpy(w, body, sizeof w);
        uint32_t rawN = w[0] - sizeof h;
        if (w[0] < sizeof h || rawN > sizeof raw || w[2] > bodyN - sizeof w) return;
        if (!mirrorLzUnpack(body + sizeof w, w[2], raw, rawN)) return;
        body = raw, bodyN = rawN;
    }
    for (uint32_t i = 0; i + sizeof(mirror_rec) <= bodyN;)
    {
        mirror_rec r;
        memcpy(&r, body + i, sizeof r);
        i += sizeof r;
        if (r.type == MREC_END || r.len > bodyN - i) break;
        if (r.type == MREC_PAD && r.len >= sizeof(mirror_pad))
        {
            padOn = body[i] != 0;
            const char* t = (const char*)body + i + sizeof(mirror_pad);
            const char* end = (const char*)body + i + r.len;
            for (int l = 0; l < 2; l++)
            {
                size_t k = strnlen(t, end - t);
                snprintf(padText[l], sizeof padText[l], "%.*s", (int)k, t);
                t += k < (size_t)(end - t) ? k + 1 : k;
            }
            padHeardAt = NowMs();
        }
        i += (r.len + 3) & ~3u;
    }
}

// false once the Sion's side is gone
static bool PadTick(uint32_t keys)
{
    if (padFd < 0) return false;
    mirror_input in = {0};
    in.magic = MIRROR_INPUT_MAGIC;
    in.keys = keys;
    in.touchX = tx, in.touchY = ty;
    if (send(padFd, &in, sizeof in, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN) return PadClose(), false;
    for (;;)
    {
        if (padN == sizeof padAcc) padN = 0;   // nothing in it made sense
        ssize_t r = recv(padFd, padAcc + padN, sizeof padAcc - padN, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN)) return PadClose(), false;
        if (r < 0) break;
        padN += r;
    }
    // bundles, with zeros between them (whole 512-byte blocks, as on USB)
    size_t i = 0;
    while (padN - i >= sizeof(mirror_hdr))
    {
        mirror_hdr h;
        memcpy(&h, padAcc + i, sizeof h);
        if (h.magic != MIRROR_MAGIC) { i += 4; continue; }
        if (h.size < sizeof h || h.size > sizeof padAcc) { i += 4; continue; }
        if (padN - i < h.size) break;
        PadBundle(padAcc + i, h.size);
        i += h.size;
    }
    memmove(padAcc, padAcc + i, padN - i);
    padN -= i;
    return true;
}

// ---------------------------------------------------------------- the networks

enum { NET_NONE, NET_HOME, NET_DS };
static int net = NET_NONE;
static char host[96];        // the Sion, as TcpConnect takes it

static void Run(const char* what, const char* dflt)
{
    const char* c = getenv(what);
    if (!c) c = dflt;
    if (!*c) return;
    Log("running %s", c);
    if (system(c)) {}
}

// The DS network: SlInit and SlJoin on a thread (SlJoin waits on scans), the screens go on
static pthread_t joiner;
static volatile int joinState;   // 0 idle, 1 joining, 2 joined, 3 failed
static volatile bool joinCancel;
static pthread_mutex_t joinLock = PTHREAD_MUTEX_INITIALIZER;
static char joinText[64];
static bool welcomed;
static sl_welcome welcome;
static char lastText[33];

static void JoinStatus(const char* t)
{
    pthread_mutex_lock(&joinLock);
    snprintf(joinText, sizeof joinText, "%s", t);
    pthread_mutex_unlock(&joinLock);
}
static bool JoinCancelled(void) { return joinCancel || quit; }

static void* JoinThread(void* a)
{
    (void)a;
    bool ok = SlInit() && SlJoin(JoinStatus);
    joinState = ok ? 2 : 3;
    return NULL;
}

static void DsNetStart(void)
{
    Run("SL_WIFI_DOWN", "modprobe rtw88_8821cs 2>/dev/null; /usr/libexec/banana/wifi down; ip link set wlan0 up");
    welcomed = false;
    lastText[0] = 0;
    joinCancel = false;
    joinState = 1;
    JoinStatus("Looking for the Sion...");
    SlSetCancel(JoinCancelled);
    pthread_create(&joiner, NULL, JoinThread, NULL);
}

static void DsNetStop(void)
{
    if (joinState == 1) joinCancel = true;
    if (joinState) pthread_join(joiner, NULL);
    if (joinState == 2) SlSend(SL_BYE, NULL, 0);
    if (joinState) SlExit();
    joinState = 0;
    welcomed = false;
}

// the Sion's IPv6 link-local address on its DS network, from its BSSID (EUI-64)
static void LinkLocal(char* out, size_t n)
{
    uint8_t b[6];
    char ssid[33];
    int freq;
    SlNetwork(b, ssid, &freq);
    const char* ifn = getenv("SL_IFACE") ? getenv("SL_IFACE") : "wlan0";
    snprintf(out, n, "[fe80::%x:%xff:fe%x:%x%%%s]", (b[0] ^ 2) << 8 | b[1], b[2], b[3], b[4] << 8 | b[5], ifn);
}

// ---------------------------------------------------------------- Download Play

static struct {
    bool active, done;
    uint32_t id, size, next, chunks, lastAck;
    char name[33];
    uint8_t* data;
    uint8_t* have;
    const char* refused;
} dl;

static void DlAck(void)
{
    sl_file_ack a = {0};
    a.id = dl.id;
    a.next = dl.next;
    a.status = dl.refused ? SL_FILE_REFUSED : dl.done ? SL_FILE_DONE : SL_FILE_RECEIVING;
    uint32_t k0 = dl.next / SL_FILE_CHUNK;
    for (uint32_t i = 0; i < 32; i++)
    {
        uint32_t k = k0 + 1 + i;
        if (k < dl.chunks && dl.have[k])
            a.ahead |= 1u << i;
    }
    SlSend(SL_FILE_ACK, &a, sizeof a);
    dl.lastAck = NowMs();
}

static void DlDrop(void)
{
    free(dl.data);
    free(dl.have);
    memset(&dl, 0, sizeof dl);
}

static void DlBegin(const sl_file_begin* b)
{
    if (dl.active && b->id == dl.id) { DlAck(); return; }
    DlDrop();
    dl.active = true;
    dl.id = b->id;
    dl.size = b->size;
    dl.chunks = (b->size + SL_FILE_CHUNK - 1) / SL_FILE_CHUNK;
    snprintf(dl.name, sizeof dl.name, "%.32s", b->name);
    if (b->size < 0x200 || b->size > SL_FILE_MAX) dl.refused = "too big for a DS";
    else
    {
        dl.data = calloc(1, b->size);
        dl.have = calloc(1, dl.chunks + 1);
        if (!dl.data || !dl.have) dl.refused = "not enough memory for it";
    }
    Log("Download Play: %s, %u bytes%s", dl.name, dl.size, dl.refused ? " (turned down)" : "");
    DlAck();
}

static void DlData(const uint8_t* body, int len)
{
    if (!dl.active || dl.done || dl.refused || len < (int)sizeof(sl_file_data)) return;
    sl_file_data d;
    memcpy(&d, body, sizeof d);
    uint32_t n = len - sizeof d, idx = d.offset / SL_FILE_CHUNK;
    if (d.id != dl.id || d.offset % SL_FILE_CHUNK || idx >= dl.chunks || n > dl.size - d.offset || dl.have[idx]) return;
    memcpy(dl.data + d.offset, body + sizeof d, n);
    dl.have[idx] = 1;
    while (dl.next < dl.size && dl.have[dl.next / SL_FILE_CHUNK]) dl.next += SL_FILE_CHUNK;
    if (dl.next > dl.size) dl.next = dl.size;
    if (dl.next == dl.size) dl.done = true;
}

static u16 Crc16(const uint8_t* p, size_t n)
{
    u16 c = 0xFFFF;
    for (size_t i = 0; i < n; i++)
    {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

// The packed image (slboot.h: header, ARM9 binary padded to 4, ARM7 binary) as a .nds the
// DS core boots
static bool DlWriteNds(const char* path)
{
    uint8_t* hd = dl.data;
    uint32_t s9, s7;
    memcpy(&s9, hd + 0x2C, 4);
    memcpy(&s7, hd + 0x3C, 4);
    uint32_t o9 = 0x200, o7 = o9 + ((s9 + 3) & ~3u);
    if (o7 + s7 > dl.size) return false;
    uint8_t h[0x200];
    memcpy(h, hd, sizeof h);
    memcpy(h + 0x20, &o9, 4);
    memcpy(h + 0x30, &o7, 4);
    memset(h + 0x68, 0, 4);   // no banner
    uint32_t total = o7 + s7;
    memcpy(h + 0x80, &total, 4);
    u16 c = Crc16(h, 0x15E);
    memcpy(h + 0x15E, &c, 2);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(h, 1, sizeof h, f);
    fwrite(hd + 0x200, 1, total - 0x200, f);
    return fclose(f) == 0;
}

// ---------------------------------------------------------------- the DS core

// Runs mirrorrun with the panels (ours closed meanwhile). fd >= 0: the mirror's connection,
// handed over. Returns its exit status once it's done (3: the Sion ended the mirror).
static int RunCore(const char* nds, int fd)
{
    Log("DS core: %s%s", nds, fd >= 0 ? " (mirror)" : "");
    ClosePanels();
    pid_t p = fork();
    if (p == 0)
    {
        char fds[16];
        if (fd >= 0)
        {
            int k = fcntl(fd, F_GETFD);
            fcntl(fd, F_SETFD, k & ~FD_CLOEXEC);
            int fl = fcntl(fd, F_GETFL);
            fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            snprintf(fds, sizeof fds, "fd:%d", fd);
            setenv("MIRROR_QUIT_ON_CLOSE", "1", 1);
            execl(mirrorrun, mirrorrun, nds, "0", "--dspico", fds, "--realtime", "1", "--jit", "1", "--view",
                  headless ? "dump:/dev/null" : "sdl", (char*)NULL);
        }
        else
            execl(mirrorrun, mirrorrun, nds, "0", "--realtime", "1", "--jit", "1", "--view",
                  headless ? "dump:/dev/null" : "sdl", (char*)NULL);
        perror(mirrorrun);
        _exit(127);
    }
    if (fd >= 0) close(fd);
    int st = 0;
    if (p > 0)
        while (waitpid(p, &st, 0) < 0 && errno == EINTR)
            if (quit) kill(p, SIGTERM);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    Log("DS core done (%d)", code);
    OpenPanels();
    return code;
}

// ---------------------------------------------------------------- home

static void ShowHome(const char* l1, const char* l2, int percent, const char* foot)
{
    static char was[320];
    char now[320];
    snprintf(now, sizeof now, "%s|%s|%d|%s", l1, l2 ? l2 : "", percent, foot ? foot : "");
    if (strcmp(now, was))
    {
        strcpy(was, now);
        uiStatus(l1, l2, percent, foot);
        if (headless) Log("screen: %s / %s / %d%% / %s", l1, l2 ? l2 : "", percent, foot ? foot : "");
    }
    Present();
}

static void OnSignal(int s) { (void)s; quit = 1; }

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
    {
        const char* o = argv[i];
        if (!strcmp(o, "--mirrorrun") && i + 1 < argc) mirrorrun = argv[++i];
        else if (!strcmp(o, "--nds") && i + 1 < argc) clientNds = argv[++i];
        else if (!strcmp(o, "--host") && i + 1 < argc) fixedHost = argv[++i];
        else if (!strcmp(o, "--dsnet")) startOnDsNet = true;
        else if (!strcmp(o, "--headless")) headless = true;
        else if (!strcmp(o, "--iface") && i + 1 < argc) SlSetInterface(argv[++i]);
        else if (!strcmp(o, "--direct-peer") && i + 1 < argc)
        {
            // tests: a veth pair to sionlinkd --if, no wpa_supplicant (sllinux.h)
            unsigned m[6];
            uint8_t mac[6];
            if (sscanf(argv[++i], "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return 2;
            for (int k = 0; k < 6; k++) mac[k] = m[k];
            SlSetDirectPeer(mac);
            setenv("SL_WIFI_DOWN", "", 0);
            setenv("SL_WIFI_UP", "", 0);
        }
        else
        {
            fprintf(stderr, "usage: %s [--mirrorrun PATH] [--nds PATH] [--host ADDR] [--dsnet] [--headless]"
                            " [--iface IF] [--direct-peer MAC]\n", argv[0]);
            return 2;
        }
    }
    signal(SIGTERM, OnSignal);
    signal(SIGINT, OnSignal);
    signal(SIGHUP, OnSignal);
    signal(SIGPIPE, SIG_IGN);
    if (!OpenPanels()) return 1;
    uiShow();

    const char* sion = fixedHost ? fixedHost : getenv("SION_HOST") ? getenv("SION_HOST") : "192.168.1.247";
    uint32_t keysWas = 0, lastKnock = 0, lastPadTry = 0, lastHomeTry = 0, frame = 0;
    bool mirrorPaused = false;   // F left the mirror: A goes back (a knock would take it again)
    char note[80] = "";
    if (startOnDsNet) net = NET_DS, DsNetStart();

    while (!quit)
    {
        uint32_t t0 = NowMs();
        uint32_t keys = Input();
        // tests: SL_TESTKEYS=MASK (libnds bits) held from the second second on, for a second
        // every two
        static const char* testKeys;
        if (!testKeys) testKeys = getenv("SL_TESTKEYS") ? getenv("SL_TESTKEYS") : "";
        if (*testKeys && frame > 60 && (frame / 60) % 2 == 0) keys |= strtoul(testKeys, NULL, 0);
        uint32_t down = keys & ~keysWas;
        keysWas = keys;

        // Y: the other network
        if (down & K_Y)
        {
            if (net == NET_DS || joinState)
            {
                DsNetStop();
                PadClose();
                Run("SL_WIFI_UP", "/usr/libexec/banana/wifi up");
                net = NET_NONE;
                lastHomeTry = 0;
            }
            else
            {
                PadClose();
                net = NET_DS;
                host[0] = 0;
                DsNetStart();
            }
            mirrorPaused = false;
        }

        // finding the Sion on the home Wi-Fi: its ssh port answers whenever it's on
        if (net == NET_NONE && t0 - lastHomeTry >= 3000)
        {
            lastHomeTry = t0;
            int fd = TcpConnect(sion, fixedHost ? PAD_PORT : 22, 1500);
            if (fd >= 0)
            {
                close(fd);
                net = NET_HOME;
                snprintf(host, sizeof host, "%s", sion);
                Log("the Sion is on the home Wi-Fi at %s", host);
            }
        }
        if (net == NET_DS && joinState == 2 && !host[0])
        {
            LinkLocal(host, sizeof host);
            Log("on the Sion's DS network; the Sion at %s", host);
        }
        if (net == NET_DS && joinState == 3)
        {
            pthread_join(joiner, NULL);
            joinState = 0;
            SlExit();
            snprintf(note, sizeof note, "Couldn't find the Sion's DS network");
            Run("SL_WIFI_UP", "/usr/libexec/banana/wifi up");
            net = NET_NONE;
            lastHomeTry = t0 + 5000;   // a moment to read it
        }

        // the Sion Link packets (DS network): welcome, texts, Download Play
        if (net == NET_DS && joinState == 2)
        {
            SlKeepAlive(0, t0);
            const sl_hdr* h;
            const uint8_t* body;
            while (SlNext(&h, &body))
            {
                if (h->type == SL_WELCOME && h->len >= sizeof(sl_welcome))
                {
                    memcpy(&welcome, body, sizeof welcome);
                    welcome.host[sizeof welcome.host - 1] = 0;
                    welcome.motd[sizeof welcome.motd - 1] = 0;
                    if (!welcomed) Log("welcomed by %s", welcome.host);
                    welcomed = true;
                }
                else if (h->type == SL_TEXT)
                    snprintf(lastText, sizeof lastText, "%.*s", h->len < 32 ? h->len : 32, (const char*)body);
                else if (h->type == SL_FILE_BEGIN && h->len >= sizeof(sl_file_begin))
                {
                    sl_file_begin b;
                    memcpy(&b, body, sizeof b);
                    DlBegin(&b);
                }
                else if (h->type == SL_FILE_DATA)
                    DlData(body, h->len);
                else if (h->type == SL_BYE)
                    welcomed = false;
            }
            if (dl.active && t0 - dl.lastAck >= 100) DlAck();
            if (dl.active && dl.done)
            {
                // all here: three acks a tenth of a second apart, then the DS core
                for (int i = 0; i < 3; i++) DlAck(), usleep(100000);
                char path[64];
                snprintf(path, sizeof path, "/tmp/sion-download.nds");
                char name[40];
                snprintf(name, sizeof name, "%s", dl.name);
                bool ok = DlWriteNds(path);
                DlDrop();
                if (ok)
                {
                    ShowHome("Starting", name, -1, NULL);
                    PadClose();
                    RunCore(path, -1);
                    uiShow();
                }
                else
                    snprintf(note, sizeof note, "Couldn't start %s", name);
            }
            if (dl.refused && t0 - dl.lastAck > 2000)
            {
                snprintf(note, sizeof note, "Couldn't start %s: %s", dl.name, dl.refused);
                DlDrop();
            }
        }

        // the Sion found: the pad, and a knock for a DS title's mirror
        if (host[0])
        {
            if (padFd < 0 && t0 - lastPadTry >= 2000)
            {
                lastPadTry = t0;
                padFd = TcpConnect(host, PAD_PORT, 1000);
                if (padFd >= 0) Log("the Sion's pad service took us");
            }
            PadTick(keys);
            if (mirrorPaused && (down & K_A)) mirrorPaused = false, lastKnock = 0;
            if (!mirrorPaused && t0 - lastKnock >= 1500 && !dl.active)
            {
                lastKnock = t0;
                int fd = TcpConnect(host, MIRROR_PORT, 400);
                if (fd >= 0)
                {
                    Log("a DS title runs on the Sion: the mirror");
                    ShowHome("Starting the game's mirror...", NULL, -1, NULL);
                    PadClose();
                    int code = RunCore(clientNds, fd);
                    uiShow();
                    // F closed it with the title still running: wait for A (a knock now
                    // would take the game straight back)
                    mirrorPaused = code != 3 && !quit;
                    keysWas = ~0u;   // the buttons that closed it stay out
                    lastKnock = NowMs();
                }
            }
        }

        // what the screens say, as the DS app's home does
        char a[96], b[96], foot[96];
        const char* l1 = "Looking for the Sion...";
        const char* l2 = note[0] ? note : NULL;
        int percent = -1;
        if (net == NET_HOME) snprintf(foot, sizeof foot, "Home Wi-Fi  -  Y: the Sion's DS network");
        else if (net == NET_DS) snprintf(foot, sizeof foot, "The Sion's DS network  -  Y: home Wi-Fi");
        else snprintf(foot, sizeof foot, "Home Wi-Fi  -  Y: the Sion's DS network");
        if (net == NET_DS && joinState == 1)
        {
            pthread_mutex_lock(&joinLock);
            snprintf(a, sizeof a, "%s", joinText);
            pthread_mutex_unlock(&joinLock);
            l1 = a;
        }
        if (dl.active && !dl.refused)
        {
            snprintf(a, sizeof a, "Receiving %s", dl.name);
            l1 = a;
            snprintf(b, sizeof b, "%u%%", dl.size ? (unsigned)((uint64_t)dl.next * 100 / dl.size) : 0);
            l2 = b;
            percent = dl.size ? (int)((uint64_t)dl.next * 100 / dl.size) : 0;
        }
        else if (mirrorPaused)
        {
            l1 = "A DS game is running on the Sion.";
            l2 = "A: back to the game";
        }
        else if (padFd >= 0 && padOn)
        {
            l1 = padText[0][0] ? padText[0] : "Controller for the Sion";
            l2 = padText[1][0] ? padText[1] : NULL;
        }
        else if (net == NET_DS && welcomed)
        {
            snprintf(a, sizeof a, "Linked to %s", welcome.host);
            l1 = a;
            l2 = lastText[0] ? lastText : welcome.motd[0] ? welcome.motd : NULL;
        }
        else if (host[0])
            l1 = "Linked to the Sion";
        if (host[0] && note[0] && !l2) l2 = note;
        ShowHome(l1, l2, percent, foot);
        if (frame++ % 600 == 599) note[0] = 0;

        uint32_t spent = NowMs() - t0;
        if (spent < 16) usleep((16 - spent) * 1000);
    }
    PadClose();
    if (net == NET_DS || joinState) DsNetStop();
    ClosePanels();
    Log("bye");
    return 0;
}
