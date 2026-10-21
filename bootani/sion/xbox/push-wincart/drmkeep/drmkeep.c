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
 * DRMKEEP=0 turns it off. Messages go to stderr with "drmkeep:".
 */
#define _GNU_SOURCE
#include <dlfcn.h>
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

static void filter(int fd, drmModeConnectorPtr c)
{
    const char *e = getenv("DRMKEEP");
    if (!c || (e && *e == '0') || !c->encoder_id || c->count_modes < 2) return;
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
