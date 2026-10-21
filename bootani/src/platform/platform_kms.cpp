//
//  platform_kms.cpp
//
//  Full-screen output with no window system: OpenGL 3.3 core through EGL on
//  a GBM surface, shown on the first connected display with KMS page flips.
//  This is what runs on a bare Linux console (for example a Buildroot image
//  that starts bootani straight from init). Only libdrm, libgbm and libEGL
//  are needed; GL entry points come from eglGetProcAddress.
//
#include "backends.h"
#include "../gfx/gl_loader.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int                 g_fd = -1;
static uint32_t            g_crtc_id;
static uint32_t            g_connector_id;
static drmModeModeInfo     g_mode;
static drmModeCrtc*        g_saved_crtc;
static struct gbm_device*  g_gbm;
static struct gbm_surface* g_gbm_surface;
static struct gbm_bo*      g_shown_bo;      // on screen now
static bool                g_crtc_set;
static EGLDisplay          g_display = EGL_NO_DISPLAY;
static EGLContext          g_context = EGL_NO_CONTEXT;
static EGLSurface          g_surface = EGL_NO_SURFACE;
static char                g_describe[160];

static bool FindOutput(int fd)
{
    drmModeRes* res = drmModeGetResources(fd);
    if (!res) return false;

    drmModeConnector* conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) conn = c;
        else if (c) drmModeFreeConnector(c);
    }
    if (!conn) { drmModeFreeResources(res); return false; }

    // KMS_MODE=WxH (e.g. 720x480 for NTSC) if the display offers it,
    // progressive before interlaced; else the display's preferred mode,
    // else the first (largest) one.
    g_mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { g_mode = conn->modes[i]; break; }
    int want_w = 0, want_h = 0;
    const char* want = getenv("KMS_MODE");
    if (want && sscanf(want, "%dx%d", &want_w, &want_h) == 2) {
        int best = -1;
        for (int i = 0; i < conn->count_modes; i++) {
            const drmModeModeInfo* m = &conn->modes[i];
            if (m->hdisplay != want_w || m->vdisplay != want_h) continue;
            if (best < 0 || ((conn->modes[best].flags & DRM_MODE_FLAG_INTERLACE) &&
                             !(m->flags & DRM_MODE_FLAG_INTERLACE)))
                best = i;
        }
        if (best >= 0) g_mode = conn->modes[best];
    }
    g_connector_id = conn->connector_id;

    // Keep the CRTC the console is using if there is one, else any CRTC the
    // connector's encoders can drive.
    g_crtc_id = 0;
    if (conn->encoder_id) {
        drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) { g_crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
    }
    for (int e = 0; e < conn->count_encoders && !g_crtc_id; e++) {
        drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc) continue;
        for (int c = 0; c < res->count_crtcs; c++)
            if (enc->possible_crtcs & (1u << c)) { g_crtc_id = res->crtcs[c]; break; }
        drmModeFreeEncoder(enc);
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return g_crtc_id != 0;
}

static int OpenCard(const char* want)
{
    if (want && *want) {
        int fd = open(want, O_RDWR | O_CLOEXEC);
        if (fd < 0) { fprintf(stderr, "bootani: cannot open %s: %s\n", want, strerror(errno)); return -1; }
        if (!FindOutput(fd)) { fprintf(stderr, "bootani: no connected display on %s\n", want); close(fd); return -1; }
        return fd;
    }
    // The first card with a connected display (render-only GPUs have none).
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        if (FindOutput(fd)) return fd;
        close(fd);
    }
    fprintf(stderr, "bootani: no DRM device with a connected display under /dev/dri\n");
    return -1;
}

static void DestroyFb(struct gbm_bo* bo, void* data)
{
    uint32_t fb = (uint32_t)(uintptr_t)data;
    if (fb) drmModeRmFB(gbm_device_get_fd(gbm_bo_get_device(bo)), fb);
}

static uint32_t FbForBo(struct gbm_bo* bo)
{
    void* data = gbm_bo_get_user_data(bo);
    if (data) return (uint32_t)(uintptr_t)data;
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
    uint32_t strides[4] = { gbm_bo_get_stride(bo) };
    uint32_t offsets[4] = { 0 };
    uint32_t fb = 0;
    if (drmModeAddFB2(g_fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo),
                      handles, strides, offsets, &fb, 0) != 0) {
        fprintf(stderr, "bootani: drmModeAddFB2 failed: %s\n", strerror(errno));
        return 0;
    }
    gbm_bo_set_user_data(bo, (void*)(uintptr_t)fb, DestroyFb);
    return fb;
}

static bool ChooseConfig(EGLConfig* out)
{
    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLint count = 0;
    if (!eglChooseConfig(g_display, attribs, NULL, 0, &count) || count <= 0) return false;
    EGLConfig* configs = (EGLConfig*)calloc(count, sizeof(EGLConfig));
    eglChooseConfig(g_display, attribs, configs, count, &count);
    // The GBM surface is XRGB8888, so the config's visual must match it.
    bool found = false;
    for (int i = 0; i < count && !found; i++) {
        EGLint id = 0;
        if (eglGetConfigAttrib(g_display, configs[i], EGL_NATIVE_VISUAL_ID, &id) && id == GBM_FORMAT_XRGB8888) {
            *out = configs[i];
            found = true;
        }
    }
    free(configs);
    return found;
}

bool Kms_Init(const PlatformConfig& cfg)
{
    g_fd = OpenCard(cfg.drm_device);
    if (g_fd < 0) return false;
    g_saved_crtc = drmModeGetCrtc(g_fd, g_crtc_id);

    g_gbm = gbm_create_device(g_fd);
    if (!g_gbm) { fprintf(stderr, "bootani: gbm_create_device failed\n"); Kms_Shutdown(); return false; }
    g_gbm_surface = gbm_surface_create(g_gbm, g_mode.hdisplay, g_mode.vdisplay, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!g_gbm_surface) { fprintf(stderr, "bootani: gbm_surface_create failed\n"); Kms_Shutdown(); return false; }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_display = get_platform_display ? get_platform_display(EGL_PLATFORM_GBM_KHR, g_gbm, NULL)
                                     : eglGetDisplay((EGLNativeDisplayType)g_gbm);
    if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, NULL, NULL)) {
        fprintf(stderr, "bootani: eglInitialize on GBM failed\n");
        Kms_Shutdown();
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "bootani: EGL has no desktop OpenGL\n");
        Kms_Shutdown();
        return false;
    }
    EGLConfig config;
    if (!ChooseConfig(&config)) {
        fprintf(stderr, "bootani: no EGL config for an XRGB8888 GBM surface\n");
        Kms_Shutdown();
        return false;
    }
    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, context_attribs);
    if (g_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "bootani: eglCreateContext (GL 3.3 core) failed: 0x%x\n", eglGetError());
        Kms_Shutdown();
        return false;
    }
    g_surface = eglCreateWindowSurface(g_display, config, (EGLNativeWindowType)g_gbm_surface, NULL);
    if (g_surface == EGL_NO_SURFACE || !eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        fprintf(stderr, "bootani: EGL window surface on GBM failed: 0x%x\n", eglGetError());
        Kms_Shutdown();
        return false;
    }
    snprintf(g_describe, sizeof(g_describe), "KMS %dx%d@%d", g_mode.hdisplay, g_mode.vdisplay, g_mode.vrefresh);
    return true;
}

void Kms_Shutdown()
{
    if (g_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_surface);
        if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
        eglTerminate(g_display);
    }
    g_display = EGL_NO_DISPLAY;
    g_context = EGL_NO_CONTEXT;
    g_surface = EGL_NO_SURFACE;
    // Give the display back to the console as we found it.
    if (g_saved_crtc) {
        if (g_crtc_set)
            drmModeSetCrtc(g_fd, g_saved_crtc->crtc_id, g_saved_crtc->buffer_id, g_saved_crtc->x,
                           g_saved_crtc->y, &g_connector_id, 1, &g_saved_crtc->mode);
        drmModeFreeCrtc(g_saved_crtc);
        g_saved_crtc = NULL;
    }
    if (g_shown_bo && g_gbm_surface) gbm_surface_release_buffer(g_gbm_surface, g_shown_bo);
    g_shown_bo = NULL;
    g_crtc_set = false;
    if (g_gbm_surface) gbm_surface_destroy(g_gbm_surface);
    if (g_gbm) gbm_device_destroy(g_gbm);
    g_gbm_surface = NULL;
    g_gbm = NULL;
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
}

void* Kms_GetProcAddress(const char* name)
{
    return (void*)eglGetProcAddress(name);
}

const char* Kms_Describe()
{
    return g_describe;
}

static void OnFlip(int, unsigned int, unsigned int, unsigned int, void* data)
{
    *(bool*)data = false;
}

void Kms_ShowFrame(unsigned int fbo, int width, int height)
{
    int dw = g_mode.hdisplay, dh = g_mode.vdisplay;
    int w = dw, h = dh, x = 0, y = 0;
    // Standard-definition TV modes (720x480 NTSC, 720x576 PAL, and their
    // 704-wide forms) are 4:3 pictures with non-square pixels. Fill them,
    // as the Xbox's video encoder stretched its 640x480 frame across the
    // whole signal. Anything else has square pixels: letterbox to 4:3.
    bool sd_tv = (dw == 720 || dw == 704) && (dh == 480 || dh == 576);
    if (!sd_tv) {
        h = dw * height / width;
        if (h > dh) { h = dh; w = dh * width / height; }
        x = (dw - w) / 2; y = (dh - h) / 2;
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, dw, dh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    // Source row 0 is the top of the image; the surface's row 0 is the bottom.
    glBlitFramebuffer(0, height, width, 0, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    eglSwapBuffers(g_display, g_surface);

    struct gbm_bo* bo = gbm_surface_lock_front_buffer(g_gbm_surface);
    if (!bo) return;
    uint32_t fb = FbForBo(bo);
    if (!fb) { gbm_surface_release_buffer(g_gbm_surface, bo); return; }

    if (!g_crtc_set) {
        if (drmModeSetCrtc(g_fd, g_crtc_id, fb, 0, 0, &g_connector_id, 1, &g_mode) != 0) {
            fprintf(stderr, "bootani: drmModeSetCrtc failed: %s\n", strerror(errno));
            gbm_surface_release_buffer(g_gbm_surface, bo);
            return;
        }
        g_crtc_set = true;
    } else {
        // Flip on vertical blank and wait for it, so the animation is paced
        // by the display like on the Xbox.
        bool pending = true;
        if (drmModePageFlip(g_fd, g_crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, &pending) == 0) {
            drmEventContext ev;
            memset(&ev, 0, sizeof(ev));
            ev.version = 2;
            ev.page_flip_handler = OnFlip;
            while (pending) {
                struct pollfd p = { g_fd, POLLIN, 0 };
                if (poll(&p, 1, 1000) <= 0) break;
                drmHandleEvent(g_fd, &ev);
            }
        } else {
            drmModeSetCrtc(g_fd, g_crtc_id, fb, 0, 0, &g_connector_id, 1, &g_mode);
        }
    }
    if (g_shown_bo) gbm_surface_release_buffer(g_gbm_surface, g_shown_bo);
    g_shown_bo = bo;
}
