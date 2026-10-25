/*
 * drmkeep: an LD_PRELOAD shim for gamescope (--backend drm) on the Sion.
 *
 * xbox-av holds the TV in 720x480 at 59.94 Hz (27.000 MHz). The TV also
 * offers 720x480 at 60.00 Hz (27.027 MHz); gamescope matches refresh rates
 * as whole numbers, sees both as "60" and takes the first, so taking the
 * display over from xbox-av was a full modeset (the HDMI-to-AV box shows
 * colour bars while it resyncs). This shim hides, from the connector's mode
 * list, every mode with the same size and scan type as the one the CRTC is
 * showing but different timings, so the only 720x480 gamescope can pick is
 * the one already on screen (and i915 can hand over without a modeset).
 *
 * Two more things gamescope does would also make i915 modeset:
 *  - it sets the connector's "content type" to Game, which changes the AVI
 *    infoframe (IT content, CN) from xbox-av's No Data: writes of that
 *    property are rewritten to DRMKEEP_CONTENT_TYPE (default 0, No Data);
 *  - on exit it turns the CRTC off (an atomic commit with ACTIVE=0), and the
 *    kernel turns it off anyway when the last scanned-out framebuffer goes
 *    away with gamescope's DRM file. So such commits are skipped, and a
 *    holder process ("drmkeep-hold", forked when gamescope makes its first
 *    framebuffer) keeps that DRM file open after gamescope is gone, so the
 *    last frame stays up until xbox-av takes the display back; wincart-run
 *    then stops it (SIGTERM; it also gives up after DRMKEEP_HOLD_S, 30 s).
 *    DRMKEEP_HOLD=0 turns this part off.
 *
 * DRMKEEP=0 turns it all off. Messages go to stderr with "drmkeep:".
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <drm.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drmMode.h>

static int same_timing(const drmModeModeInfo *a, const drmModeModeInfo *b)
{
    return a->clock == b->clock && a->hdisplay == b->hdisplay && a->hsync_start == b->hsync_start &&
           a->hsync_end == b->hsync_end && a->htotal == b->htotal && a->vdisplay == b->vdisplay &&
           a->vsync_start == b->vsync_start && a->vsync_end == b->vsync_end && a->vtotal == b->vtotal &&
           (a->flags & ~DRM_MODE_FLAG_PIC_AR_MASK) == (b->flags & ~DRM_MODE_FLAG_PIC_AR_MASK);
}

static int off(void);

static void filter(int fd, drmModeConnectorPtr c)
{
    if (!c || off() || !c->encoder_id || c->count_modes < 2) return;
    drmModeEncoderPtr enc = drmModeGetEncoder(fd, c->encoder_id);
    if (!enc) return;
    drmModeCrtcPtr crtc = enc->crtc_id ? drmModeGetCrtc(fd, enc->crtc_id) : NULL;
    drmModeFreeEncoder(enc);
    if (!crtc) return;
    if (crtc->mode_valid) {
        const drmModeModeInfo cur = crtc->mode;
        int n = 0, cur_at = -1;
        for (int i = 0; i < c->count_modes; i++) {
            const drmModeModeInfo *m = &c->modes[i];
            int same_kind = m->hdisplay == cur.hdisplay && m->vdisplay == cur.vdisplay &&
                            (m->flags & DRM_MODE_FLAG_INTERLACE) == (cur.flags & DRM_MODE_FLAG_INTERLACE);
            if (same_kind && !same_timing(m, &cur)) {
                fprintf(stderr, "drmkeep: hiding %s (%u kHz), keeping the current %s (%u kHz)\n", m->name, m->clock, cur.name, cur.clock);
                continue;
            }
            if (same_kind) cur_at = n;
            c->modes[n++] = *m;
        }
        if (cur_at < 0 && n < c->count_modes) {
            /* The current mode isn't in the list (a user mode): offer it. */
            c->modes[n] = cur;
            cur_at = n++;
            fprintf(stderr, "drmkeep: offering the current %s (%u kHz)\n", cur.name, cur.clock);
        }
        c->count_modes = n;
    }
    drmModeFreeCrtc(crtc);
}

drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t id)
{
    static drmModeConnectorPtr (*real)(int, uint32_t);
    if (!real) real = (drmModeConnectorPtr (*)(int, uint32_t))dlsym(RTLD_NEXT, "drmModeGetConnector");
    drmModeConnectorPtr c = real(fd, id);
    filter(fd, c);
    return c;
}

drmModeConnectorPtr drmModeGetConnectorCurrent(int fd, uint32_t id)
{
    static drmModeConnectorPtr (*real)(int, uint32_t);
    if (!real) real = (drmModeConnectorPtr (*)(int, uint32_t))dlsym(RTLD_NEXT, "drmModeGetConnectorCurrent");
    drmModeConnectorPtr c = real(fd, id);
    filter(fd, c);
    return c;
}

/* ---- properties ---------------------------------------------------------- */

static int off(void)
{
    const char *e = getenv("DRMKEEP");
    return e && *e == '0';
}

static int hold_off(void)
{
    const char *e = getenv("DRMKEEP_HOLD");
    return off() || (e && *e == '0');
}

enum { MAXP = 64 };
static uint32_t content_type_[MAXP], active_[MAXP];
static int n_content_type_, n_active_;

static int in(const uint32_t *a, int n, uint32_t v)
{
    for (int i = 0; i < n; i++) if (a[i] == v) return 1;
    return 0;
}

drmModePropertyPtr drmModeGetProperty(int fd, uint32_t id)
{
    static drmModePropertyPtr (*real)(int, uint32_t);
    if (!real) real = (drmModePropertyPtr (*)(int, uint32_t))dlsym(RTLD_NEXT, "drmModeGetProperty");
    drmModePropertyPtr p = real(fd, id);
    if (p) {
        if (!strcmp(p->name, "content type") && !in(content_type_, n_content_type_, id) && n_content_type_ < MAXP)
            content_type_[n_content_type_++] = id;
        if (!strcmp(p->name, "ACTIVE") && !in(active_, n_active_, id) && n_active_ < MAXP)
            active_[n_active_++] = id;
    }
    return p;
}

/* Requests that turn a CRTC off (ACTIVE=0), with the CRTC. */
enum { MAXR = 16 };
static drmModeAtomicReqPtr blanking_[MAXR];
static uint32_t blank_crtc_[MAXR];

int drmModeAtomicAddProperty(drmModeAtomicReqPtr req, uint32_t object_id, uint32_t property_id, uint64_t value)
{
    static int (*real)(drmModeAtomicReqPtr, uint32_t, uint32_t, uint64_t);
    if (!real) real = (int (*)(drmModeAtomicReqPtr, uint32_t, uint32_t, uint64_t))dlsym(RTLD_NEXT, "drmModeAtomicAddProperty");
    if (!off() && in(content_type_, n_content_type_, property_id)) {
        const char *e = getenv("DRMKEEP_CONTENT_TYPE");
        uint64_t keep = e ? strtoull(e, 0, 0) : 0;
        if (value != keep) {
            static int said;
            if (!said++) fprintf(stderr, "drmkeep: content type %llu -> %llu\n", (unsigned long long)value, (unsigned long long)keep);
            value = keep;
        }
    }
    if (!hold_off() && value == 0 && in(active_, n_active_, property_id)) {
        for (int i = 0; i < MAXR; i++)
            if (!blanking_[i]) { blanking_[i] = req; blank_crtc_[i] = object_id; break; }
    }
    return real(req, object_id, property_id, value);
}

/* Whether req turns off a CRTC that is on now (gamescope also turns off the
   CRTCs it doesn't use, which are off already: those commits go through).
   fd < 0: just forget req. */
static int take_blanking(int fd, drmModeAtomicReqPtr req)
{
    int hit = 0;
    for (int i = 0; i < MAXR; i++) {
        if (!req || blanking_[i] != req) continue;
        if (fd >= 0 && !hit) {
            drmModeCrtcPtr c = drmModeGetCrtc(fd, blank_crtc_[i]);
            if (c && c->mode_valid) hit = 1;
            if (c) drmModeFreeCrtc(c);
        }
        blanking_[i] = 0;
    }
    return hit;
}

int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *user_data)
{
    static int (*real)(int, drmModeAtomicReqPtr, uint32_t, void *);
    if (!real) real = (int (*)(int, drmModeAtomicReqPtr, uint32_t, void *))dlsym(RTLD_NEXT, "drmModeAtomicCommit");
    if (take_blanking(fd, req)) {
        fprintf(stderr, "drmkeep: skipping a commit that turns the display off%s\n",
                (flags & DRM_MODE_ATOMIC_TEST_ONLY) ? " (test)" : "");
        return 0;
    }
    return real(fd, req, flags, user_data);
}

void drmModeAtomicFree(drmModeAtomicReqPtr req)
{
    static void (*real)(drmModeAtomicReqPtr);
    if (!real) real = (void (*)(drmModeAtomicReqPtr))dlsym(RTLD_NEXT, "drmModeAtomicFree");
    take_blanking(-1, req);
    real(req);
}

/* ---- the holder ------------------------------------------------------------ */

static void start_holder(int fd)
{
    static int started;
    if (started || hold_off()) return;
    started = 1;
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid != 0) {
        if (pid > 0) fprintf(stderr, "drmkeep: holder %d keeps the display's framebuffers past exit\n", (int)pid);
        return;
    }
    /* The child: only async-signal-safe calls from here. */
    prctl(PR_SET_NAME, "drmkeep-hold", 0, 0, 0);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    for (int i = 0; i < 1024; i++) if (i != fd && i != 2) close(i);
    const char *e = getenv("DRMKEEP_HOLD_S");
    int limit = e ? atoi(e) : 30;
    struct timespec t = { 0, 100000000 };
    while (getppid() == parent) nanosleep(&t, 0);
    /* gamescope is gone: let go of master (seatd does too), keep the file. */
    ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
    for (int i = 0; i < limit * 10; i++) nanosleep(&t, 0);
    _exit(0);
}

#define WRAP_ADDFB(name, proto, args) \
    int name proto \
    { \
        static int (*real) proto; \
        if (!real) real = (int (*) proto)dlsym(RTLD_NEXT, #name); \
        int r = real args; \
        if (!r) start_holder(fd); \
        return r; \
    }

WRAP_ADDFB(drmModeAddFB2WithModifiers,
           (int fd, uint32_t width, uint32_t height, uint32_t pixel_format, const uint32_t bo_handles[4],
            const uint32_t pitches[4], const uint32_t offsets[4], const uint64_t modifier[4], uint32_t *buf_id, uint32_t flags),
           (fd, width, height, pixel_format, bo_handles, pitches, offsets, modifier, buf_id, flags))
WRAP_ADDFB(drmModeAddFB2,
           (int fd, uint32_t width, uint32_t height, uint32_t pixel_format, const uint32_t bo_handles[4],
            const uint32_t pitches[4], const uint32_t offsets[4], uint32_t *buf_id, uint32_t flags),
           (fd, width, height, pixel_format, bo_handles, pitches, offsets, buf_id, flags))
