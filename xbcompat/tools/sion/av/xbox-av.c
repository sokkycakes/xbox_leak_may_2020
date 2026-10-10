/*
 * xbox-av: keeps the Xbox's picture on the TV between titles.
 *
 * On the Xbox a title switch never shows anything but the last frame or
 * black. The kernel's AV state survives the quick reboot of
 * XLaunchNewImage (private/ntos/av/modeset.c, AvSetSavedDataAddress), the
 * old title's D3DDevice_PersistDisplay leaves a copy of its last frame on
 * screen (private/windows/directx/dxg/d3d8/se/d3dbase.cpp), and the next
 * title's first Present frees that copy and takes over
 * (present.cpp, FirstFlip). Without PersistDisplay the screen is blanked
 * (mpcore.cpp, ShutdownEngines). The video mode is never reprogrammed.
 *
 * On the Sion every title is its own xbcompat process, and KMS hands the
 * display back to the Linux console whenever its DRM master goes away: a
 * flash of the tty and a mode change per launch. xbox-av plays the part of
 * the Xbox's kernel AV state. It runs for the whole Xbox session, keeps the
 * DRM device open (so the console never takes the display back) and the
 * VT in graphics mode, and holds the frame the last title left:
 *
 *   'T'                    a title is starting: xbox-av lets go of DRM
 *                          master, the frame it shows stays on screen
 *                          until the title's first Present
 *   'S' w h pixels         a title is leaving (it has dropped master): put
 *                          w x h XRGB8888 pixels (top row first; 0 x 0 for
 *                          black) on screen in the current mode and keep
 *                          them there
 *
 * Each command is answered with 'k'. A title that goes away after 'T'
 * without 'S' (a crash, a kill) leaves black behind.
 *
 *   xbox-av [--socket PATH]     (default $XBOX_AV_SOCKET, /run/xbox-av.sock)
 *
 * KMS_MODE=WxH picks the mode the session starts in, as bootani does.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
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

static int drm = -1, tty = -1;
static uint32_t conn_id, crtc_id;
static struct drm_mode_modeinfo mode;
static int have_mode;
static struct { uint32_t fb, handle, pitch, w, h; uint64_t size; uint8_t *map; } shown;
static enum { OWN, LENT, WAIT } state = WAIT;
static volatile sig_atomic_t quit;

static void say(const char *fmt, ...)
{
    va_list ap;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(stderr, "[%5ld.%03ld] xbox-av: ", (long)t.tv_sec, t.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int xioctl(unsigned long req, void *arg)
{
    int r;
    do r = ioctl(drm, req, arg); while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static int open_card(void)
{
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct drm_mode_card_res res = { 0 };
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 && res.count_connectors && res.count_crtcs) {
            say("using %s", path);
            return fd;
        }
        close(fd);
    }
    return -1;
}

/* The connected connector and the CRTC that drives it (or can), and the
   mode to start in: KMS_MODE if the display has it, else the CRTC's, else
   the display's preferred one. */
static int find_output(void)
{
    struct drm_mode_card_res res = { 0 };
    if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res)) return -1;
    uint32_t conns[res.count_connectors + 1], crtcs[res.count_crtcs + 1], encs[res.count_encoders + 1];
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = 0;
    res.count_fbs = 0;
    if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res)) return -1;

    for (uint32_t c = 0; c < res.count_connectors; c++) {
        struct drm_mode_get_connector gc = { .connector_id = conns[c] };
        if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &gc)) continue;
        if (gc.connection != 1 /* connected */ || !gc.count_modes) continue;
        uint32_t nmodes = gc.count_modes, nenc = gc.count_encoders;
        struct drm_mode_modeinfo *modes = calloc(nmodes + 1, sizeof *modes);
        uint32_t *cencs = calloc(nenc + 1, sizeof *cencs);
        gc.modes_ptr = (uintptr_t)modes;
        gc.encoders_ptr = (uintptr_t)cencs;
        gc.count_props = 0;
        gc.props_ptr = gc.prop_values_ptr = 0;
        if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &gc)) { free(modes); free(cencs); continue; }
        if (gc.count_modes < nmodes) nmodes = gc.count_modes;
        if (gc.count_encoders < nenc) nenc = gc.count_encoders;

        uint32_t crtc = 0;
        if (gc.encoder_id) {
            struct drm_mode_get_encoder ge = { .encoder_id = gc.encoder_id };
            if (!xioctl(DRM_IOCTL_MODE_GETENCODER, &ge)) crtc = ge.crtc_id;
        }
        for (uint32_t e = 0; !crtc && e < nenc; e++) {
            struct drm_mode_get_encoder ge = { .encoder_id = cencs[e] };
            if (xioctl(DRM_IOCTL_MODE_GETENCODER, &ge)) continue;
            for (uint32_t k = 0; k < res.count_crtcs; k++)
                if (ge.possible_crtcs & (1u << k)) { crtc = crtcs[k]; break; }
        }
        if (!crtc) { free(modes); free(cencs); continue; }

        struct drm_mode_crtc gcr = { .crtc_id = crtc };
        int cur = !xioctl(DRM_IOCTL_MODE_GETCRTC, &gcr) && gcr.mode_valid;
        int pick = -1, ww = 0, wh = 0;
        const char *want = getenv("KMS_MODE");
        if (want && sscanf(want, "%dx%d", &ww, &wh) == 2) {
            for (int pass = 0; pass < 2 && pick < 0; pass++)
                for (uint32_t m = 0; m < nmodes; m++)
                    if (modes[m].hdisplay == ww && modes[m].vdisplay == wh &&
                        !!(modes[m].flags & DRM_MODE_FLAG_INTERLACE) == pass) { pick = m; break; }
        }
        if (pick >= 0) mode = modes[pick];
        else if (cur) mode = gcr.mode;
        else {
            mode = modes[0];
            for (uint32_t m = 0; m < nmodes; m++)
                if (modes[m].type & DRM_MODE_TYPE_PREFERRED) { mode = modes[m]; break; }
        }
        have_mode = 1;
        conn_id = conns[c];
        crtc_id = crtc;
        say("connector %u on CRTC %u, %ux%u%s", conn_id, crtc_id, mode.hdisplay, mode.vdisplay,
            mode.flags & DRM_MODE_FLAG_INTERLACE ? "i" : "");
        free(modes);
        free(cencs);
        return 0;
    }
    say("no connected display");
    return -1;
}

static int set_master(int tries_ms)
{
    for (int t = 0;; t += 10) {
        if (ioctl(drm, DRM_IOCTL_SET_MASTER, 0) == 0) return 0;
        if (t >= tries_ms) return -1;
        sleep_ms(10);
    }
}

static void drop_master(void)
{
    if (ioctl(drm, DRM_IOCTL_DROP_MASTER, 0)) say("dropping DRM master: %s", strerror(errno));
}

static void free_fb(uint32_t fb, uint32_t handle, uint8_t *map, uint64_t size)
{
    if (map) munmap(map, size);
    if (fb) xioctl(DRM_IOCTL_MODE_RMFB, &fb);
    if (handle) {
        struct drm_mode_destroy_dumb dd = { .handle = handle };
        xioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
}

/* Put pixels (w x h XRGB8888, top row first; NULL for black) on screen in
   the current mode, scaled to it. Needs DRM master. */
static int show(const uint8_t *px, uint32_t w, uint32_t h)
{
    if (!crtc_id && find_output()) return -1;
    struct drm_mode_crtc gcr = { .crtc_id = crtc_id };
    if (!xioctl(DRM_IOCTL_MODE_GETCRTC, &gcr) && gcr.mode_valid) { mode = gcr.mode; have_mode = 1; }
    if (!have_mode && find_output()) return -1;

    struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
    if (xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { say("CREATE_DUMB: %s", strerror(errno)); return -1; }
    struct drm_mode_fb_cmd fc = { .width = cd.width, .height = cd.height, .pitch = cd.pitch, .bpp = 32,
                                  .depth = 24, .handle = cd.handle };
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    uint8_t *map = MAP_FAILED;
    if (xioctl(DRM_IOCTL_MODE_ADDFB, &fc) || xioctl(DRM_IOCTL_MODE_MAP_DUMB, &md) ||
        (map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, md.offset)) == MAP_FAILED) {
        say("framebuffer: %s", strerror(errno));
        free_fb(fc.fb_id, cd.handle, NULL, 0);
        return -1;
    }
    for (uint32_t y = 0; y < cd.height; y++) {
        uint32_t *row = (uint32_t *)(map + (size_t)y * cd.pitch);
        if (!px || !w || !h) { memset(row, 0, cd.width * 4); continue; }
        const uint32_t *src = (const uint32_t *)(px + (size_t)(y * h / cd.height) * w * 4);
        if (w == cd.width) memcpy(row, src, w * 4);
        else for (uint32_t x = 0; x < cd.width; x++) row[x] = src[x * w / cd.width];
    }

    struct drm_mode_crtc sc = { .crtc_id = crtc_id, .fb_id = fc.fb_id, .set_connectors_ptr = (uintptr_t)&conn_id,
                                .count_connectors = 1, .mode = mode, .mode_valid = 1 };
    if (xioctl(DRM_IOCTL_MODE_SETCRTC, &sc)) {
        say("SETCRTC: %s", strerror(errno));
        free_fb(fc.fb_id, cd.handle, map, cd.size);
        crtc_id = 0;    /* look for the display again next time */
        return -1;
    }
    free_fb(shown.fb, shown.handle, shown.map, shown.size);
    shown.fb = fc.fb_id; shown.handle = cd.handle; shown.pitch = cd.pitch;
    shown.w = cd.width; shown.h = cd.height; shown.size = cd.size; shown.map = map;
    return 0;
}

/* Take the display from whoever let go of it (or died) and show px. */
static void take_over(const uint8_t *px, uint32_t w, uint32_t h, int wait_ms)
{
    if (set_master(wait_ms)) {
        if (state != WAIT) say("the display is someone else's; waiting for it");
        state = WAIT;
        return;
    }
    state = OWN;
    if (show(px, w, h) == 0) say(px ? "holding a %ux%u frame" : "holding black", w, h);
}

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= r;
    }
    return 0;
}

static void on_signal(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
    const char *sock = getenv("XBOX_AV_SOCKET");
    if (argc == 3 && !strcmp(argv[1], "--socket")) sock = argv[2];
    else if (argc != 1) { fprintf(stderr, "usage: %s [--socket PATH]\n", argv[0]); return 2; }
    if (!sock || !*sock) sock = "/run/xbox-av.sock";

    drm = open_card();
    if (drm < 0) { say("no KMS device"); return 1; }
    find_output();

    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    unlink(sock);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 4)) {
        say("%s: %s", sock, strerror(errno));
        return 1;
    }

    /* The console mustn't draw over the picture, blank it, or (through
       fbdev) take the display back. */
    tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty >= 0 && ioctl(tty, KDSETMODE, KD_GRAPHICS)) { close(tty); tty = -1; }

    struct sigaction act = { .sa_handler = on_signal };
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);
    signal(SIGPIPE, SIG_IGN);

    take_over(NULL, 0, 0, 0);   /* black in the session's mode, if the display is free */

    int cl = -1;
    uint8_t *px = NULL;
    size_t pxcap = 0;
    while (!quit) {
        struct pollfd p[2] = { { ls, POLLIN, 0 }, { cl, POLLIN, 0 } };
        int r = poll(p, cl >= 0 ? 2 : 1, state == WAIT ? 200 : -1);
        if (r < 0) continue;
        if (r == 0) {   /* someone we don't know (bootani) has the display: take it when they're done */
            if (cl < 0) take_over(NULL, 0, 0, 0);
            continue;
        }
        if (p[0].revents & POLLIN) {
            int n = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
            if (n >= 0) {
                if (cl >= 0) close(cl);   /* the old title is gone */
                cl = n;
            }
            continue;
        }
        if (cl < 0 || !(p[1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        char cmd;
        if (read_full(cl, &cmd, 1)) {
            close(cl);
            cl = -1;
            if (state == LENT) {
                say("the title went away without handing the display back");
                take_over(NULL, 0, 0, 3000);
            }
            continue;
        }
        if (cmd == 'T') {
            if (state == OWN) drop_master();
            state = LENT;
            say("a title is starting");
        } else if (cmd == 'S') {
            uint32_t wh[2];
            if (read_full(cl, wh, sizeof wh) || wh[0] > 4096 || wh[1] > 4096) { close(cl); cl = -1; continue; }
            size_t n = (size_t)wh[0] * wh[1] * 4;
            if (n > pxcap) { free(px); px = malloc(n); pxcap = px ? n : 0; }
            if (n && (!px || read_full(cl, px, n))) { close(cl); cl = -1; continue; }
            take_over(n ? px : NULL, wh[0], wh[1], 1000);
        } else {
            say("unknown command %#x", cmd);
            close(cl);
            cl = -1;
            continue;
        }
        if (write(cl, "k", 1) != 1) { close(cl); cl = -1; }
    }

    say("stopping");
    if (state == OWN) drop_master();
    if (tty >= 0) ioctl(tty, KDSETMODE, KD_TEXT);
    unlink(sock);
    return 0;
}
