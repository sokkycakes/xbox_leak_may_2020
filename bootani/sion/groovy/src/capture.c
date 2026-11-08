/*
 * Screen capture for sion-groovy: finds the plane the Sion is scanning out,
 * imports its buffer into EGL as a dma-buf (any tiling or compression the
 * driver picked) and lets the GPU shrink it to 720x480. Nothing here changes
 * the display; it only reads the current framebuffer.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define W 720
#define H 480

static int card = -1, crtc_index;
static uint32_t crtc_id, plane_id;
static EGLDisplay dpy;
static GLuint prog, fbo, rbo;
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
static uint8_t rgba[W * H * 4];

struct cached { uint32_t fb; EGLImageKHR img; GLuint tex; int w, h; };
static struct cached cache[4];

static int find_plane(int verbose) {
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        drmModeResPtr res = drmModeGetResources(fd);
        drmModePlaneResPtr pr = drmModeGetPlaneResources(fd);
        if (res && pr) {
            for (int c = 0; c < res->count_crtcs && card < 0; c++) {
                drmModeCrtcPtr cr = drmModeGetCrtc(fd, res->crtcs[c]);
                if (cr && cr->mode_valid && cr->buffer_id) {
                    for (uint32_t p = 0; p < pr->count_planes; p++) {
                        drmModePlanePtr pl = drmModeGetPlane(fd, pr->planes[p]);
                        if (pl && pl->crtc_id == cr->crtc_id && pl->fb_id == cr->buffer_id) {
                            card = fd; crtc_id = cr->crtc_id; crtc_index = c; plane_id = pl->plane_id;
                            if (verbose) fprintf(stderr, "%s crtc %u plane %u mode %ux%u%s\n", path, crtc_id,
                                                 plane_id, cr->mode.hdisplay, cr->mode.vdisplay,
                                                 (cr->mode.flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "");
                        }
                        drmModeFreePlane(pl);
                    }
                }
                drmModeFreeCrtc(cr);
            }
        }
        drmModeFreePlaneResources(pr);
        drmModeFreeResources(res);
        if (card >= 0) return 0;
        close(fd);
    }
    fprintf(stderr, "no active display plane found (need root)\n");
    return -1;
}

static const char *vs_src =
    "attribute vec2 p; varying vec2 uv;"
    "void main(){ uv = vec2(p.x * 0.5 + 0.5, 0.5 + p.y * 0.5); gl_Position = vec4(p, 0.0, 1.0); }";
/* average 4 source texels across each output pixel (2880 -> 720 is exactly 4) */
static const char *fs_head_ext =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision highp float; varying vec2 uv; uniform samplerExternalOES t; uniform float dx;";
static const char *fs_head_2d =
    "precision highp float; varying vec2 uv; uniform sampler2D t; uniform float dx;";
static const char *fs_body =
    "void main(){ vec4 c = texture2D(t, uv + vec2(-1.5*dx,0.0)) + texture2D(t, uv + vec2(-0.5*dx,0.0))"
    " + texture2D(t, uv + vec2(0.5*dx,0.0)) + texture2D(t, uv + vec2(1.5*dx,0.0)); gl_FragColor = vec4(c.rgb * 0.25, 1.0); }";

static GLuint shader2(GLenum type, const char *a, const char *b) {
    GLuint s = glCreateShader(type);
    const char *src[2] = { a, b };
    glShaderSource(s, 2, src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log); fprintf(stderr, "shader: %s\n", log); }
    return s;
}

int gl_init(int verbose, int selftest) {
    int rfd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm = rfd >= 0 ? gbm_create_device(rfd) : NULL;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (gbm && gpd) dpy = gpd(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (!dpy && gpd) dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (!dpy || !eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "EGL init failed\n"); return -1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "EGL context failed\n"); return -1; }
    if (verbose) fprintf(stderr, "GL: %s\n", glGetString(GL_RENDERER));
    create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    image_target = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    prog = glCreateProgram();
    glAttachShader(prog, shader2(GL_VERTEX_SHADER, vs_src, ""));
    glAttachShader(prog, shader2(GL_FRAGMENT_SHADER, selftest ? fs_head_2d : fs_head_ext, fs_body));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    glUseProgram(prog);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO incomplete\n"); return -1; }
    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, W, H);
    return 0;
}

/* draw texture `tex` (w wide) into the 720x480 target and read it back as BGR */
void gl_shrink(GLuint tex, GLenum target, int w, uint8_t *bgr) {
    glBindTexture(target, tex);
    glUniform1f(glGetUniformLocation(prog, "dx"), (float)w / W / 4.0f / (float)w);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* readback row 0 is the bottom of the FBO, which the quad maps to the top of the source */
    for (int i = 0; i < W * H; i++) {
        bgr[i * 3 + 0] = rgba[i * 4 + 2];
        bgr[i * 3 + 1] = rgba[i * 4 + 1];
        bgr[i * 3 + 2] = rgba[i * 4 + 0];
    }
}

static struct cached *import_fb(uint32_t fb_id) {
    for (int i = 0; i < 4; i++) if (cache[i].fb == fb_id && cache[i].img) return &cache[i];
    drmModeFB2Ptr fb = drmModeGetFB2(card, fb_id);
    if (!fb || !fb->handles[0]) { fprintf(stderr, "GETFB2 failed (need root)\n"); drmModeFreeFB2(fb); return NULL; }
    EGLint a[64]; int n = 0;
    a[n++] = EGL_WIDTH; a[n++] = fb->width;
    a[n++] = EGL_HEIGHT; a[n++] = fb->height;
    a[n++] = EGL_LINUX_DRM_FOURCC_EXT; a[n++] = fb->pixel_format;
    int fds[4] = { -1, -1, -1, -1 };
    for (int p = 0; p < 4 && fb->handles[p]; p++) {
        drmPrimeHandleToFD(card, fb->handles[p], O_CLOEXEC, &fds[p]);
        static const EGLint keys[4][5] = {
            { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
            { EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
            { EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
            { EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT } };
        a[n++] = keys[p][0]; a[n++] = fds[p];
        a[n++] = keys[p][1]; a[n++] = fb->offsets[p];
        a[n++] = keys[p][2]; a[n++] = fb->pitches[p];
        if (fb->flags & DRM_MODE_FB_MODIFIERS) {
            a[n++] = keys[p][3]; a[n++] = (EGLint)(fb->modifier & 0xffffffff);
            a[n++] = keys[p][4]; a[n++] = (EGLint)(fb->modifier >> 32);
        }
    }
    a[n++] = EGL_NONE;
    EGLImageKHR img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    for (int p = 0; p < 4; p++) if (fds[p] >= 0) close(fds[p]);
    for (int p = 0; p < 4 && fb->handles[p]; p++) {
        int dup = 0;
        for (int q = 0; q < p; q++) if (fb->handles[q] == fb->handles[p]) dup = 1;
        if (!dup) drmCloseBufferHandle(card, fb->handles[p]);
    }
    if (!img) {
        fprintf(stderr, "EGL import failed: %ux%u fourcc %.4s modifier 0x%llx\n", fb->width, fb->height,
                (char *)&fb->pixel_format, (unsigned long long)fb->modifier);
        drmModeFreeFB2(fb);
        return NULL;
    }
    static int next;
    struct cached *c = &cache[next++ % 4];
    if (c->img) { destroy_image(dpy, c->img); glDeleteTextures(1, &c->tex); }
    c->fb = fb_id; c->img = img; c->w = fb->width; c->h = fb->height;
    glGenTextures(1, &c->tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, c->tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    image_target(GL_TEXTURE_EXTERNAL_OES, img);
    drmModeFreeFB2(fb);
    return c;
}

int capture_init(int verbose) {
    if (find_plane(verbose)) return -1;
    return gl_init(verbose, 0);
}

void capture_wait_vblank(void) {
    drmVBlank vb;
    memset(&vb, 0, sizeof vb);
    vb.request.type = DRM_VBLANK_RELATIVE | (crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT);
    vb.request.sequence = 1;
    drmWaitVBlank(card, &vb);
}

int capture_frame(uint8_t *bgr, int verbose) {
    drmModePlanePtr pl = drmModeGetPlane(card, plane_id);
    uint32_t fb = pl ? pl->fb_id : 0;
    drmModeFreePlane(pl);
    if (!fb) return 1;   /* plane off for a moment (mode change); skip */
    struct cached *c = import_fb(fb);
    if (!c) return -1;
    (void)verbose;
    gl_shrink(c->tex, GL_TEXTURE_EXTERNAL_OES, c->w, bgr);
    return 0;
}

/* Cloud self test: no KMS, so upload a 2880x480 picture as a normal texture
   and run the same shrink shader on it. */
int capture_selftest(const uint8_t *rgb2880, uint8_t *bgr) {
    if (gl_init(1, 1)) return -1;
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2880, H, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb2880);
    gl_shrink(t, GL_TEXTURE_2D, 2880, bgr);
    return 0;
}
