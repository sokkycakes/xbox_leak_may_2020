/*
 * xbcompat-gpu: xemu's NV2A model hosted inside xbcompat.
 *
 * This file is the device side: it owns the NV2AState, stands in for the
 * QEMU services the xemu sources call (threads, timers, the big lock, the
 * memory API's dirty tracking, the VGA register file), dispatches register
 * accesses to xemu's blocks, and draws the CRTC's framebuffer to the SDL
 * window at each vblank.  gpu_glue.c is the xbcompat side (traps, the
 * interrupt line, the kernel's video calls) and talks to this file only
 * through gpu_host.h.
 *
 * Built only into the GPL test executable xbcompat-gpu (make GPU=1).
 */
#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-notifications.h"

#include <SDL.h>
#include <epoxy/gl.h>
#include <sys/mman.h>

#include "gpu_host.h"

/* ---- QEMU stand-ins ----------------------------------------------------- */

Error *error_fatal;
struct xbc_xemu_config g_config = {
    .display = { .renderer = CONFIG_DISPLAY_RENDERER_OPENGL, .quality = { .surface_scale = 1 } },
    .perf = { .cache_shaders = false },
};

void error_setg_internal(Error **errp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (errp == &error_fatal) {
        gpu_log("nv2a: fatal: %s", msg);
        abort();
    }
    if (errp) {
        *errp = g_new0(Error, 1);
        (*errp)->msg = msg;
    } else {
        g_free(msg);
    }
}

void error_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gpu_log("nv2a: %s", msg);
    g_free(msg);
}

void xemu_queue_notification(const char *msg) { gpu_log("nv2a: %s", msg); }
void xemu_queue_error_message(const char *msg) { gpu_log("nv2a: error: %s", msg); }

const char *xemu_settings_get_base_path(void)
{
    static char path[512];
    if (!path[0]) {
        const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
        snprintf(path, sizeof path, "%s%s/xbcompat-gpu/", cache ? cache : home ? home : "/tmp",
                 cache ? "" : home ? "/.cache" : "");
        g_mkdir_with_parents(path, 0755);
    }
    return path;
}

void rcu_register_thread(void) {}
void rcu_unregister_thread(void) {}

struct thread_start { void *(*fn)(void *); void *arg; char name[16]; };

static void *thread_trampoline(void *p)
{
    struct thread_start s = *(struct thread_start *)p;
    g_free(p);
    pthread_setname_np(pthread_self(), s.name);
    gpu_thread_started(s.name);
    return s.fn(s.arg);
}

void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode)
{
    struct thread_start *s = g_new0(struct thread_start, 1);
    s->fn = fn;
    s->arg = arg;
    snprintf(s->name, sizeof s->name, "%s", name);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    if (mode == QEMU_THREAD_DETACHED) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->t, &attr, thread_trampoline, s)) abort();
    pthread_attr_destroy(&attr);
}

bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == 0;
}

/* The big lock orders register accesses (trapped from guest threads and the
   interrupt thread), vblank and the device's own interrupt updates, as the
   BQL does around MMIO in QEMU. */
static pthread_mutex_t bql = PTHREAD_MUTEX_INITIALIZER;
static __thread int bql_depth;
void bql_lock(void) { pthread_mutex_lock(&bql); bql_depth++; }
void bql_unlock(void) { bql_depth--; pthread_mutex_unlock(&bql); }
bool bql_locked(void) { return bql_depth > 0; }

/* Timers (PTIMER's alarm) on one thread. */
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cond = PTHREAD_COND_INITIALIZER;
static QEMUTimer *timers;
static bool timer_thread_running;

int64_t qemu_clock_get_ns(QEMUClockType type)
{
    (void)type;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void *timer_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&timer_lock);
    for (;;) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), next = INT64_MAX;
        QEMUTimer *due = NULL;
        for (QEMUTimer *t = timers; t; t = t->next) {
            if (t->expire < 0) continue;
            if (t->expire <= now) { due = t; break; }
            if (t->expire < next) next = t->expire;
        }
        if (due) {
            due->expire = -1;
            pthread_mutex_unlock(&timer_lock);
            bql_lock();
            due->cb(due->opaque);
            bql_unlock();
            pthread_mutex_lock(&timer_lock);
            continue;
        }
        if (next == INT64_MAX) {
            pthread_cond_wait(&timer_cond, &timer_lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t when = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + (next - now);
            ts.tv_sec = when / 1000000000LL;
            ts.tv_nsec = when % 1000000000LL;
            pthread_cond_timedwait(&timer_cond, &timer_lock, &ts);
        }
    }
    return NULL;
}

void timer_init_ns(QEMUTimer *t, QEMUClockType type, QEMUTimerCB *cb, void *opaque)
{
    (void)type;
    t->cb = cb;
    t->opaque = opaque;
    t->expire = -1;
    pthread_mutex_lock(&timer_lock);
    t->next = timers;
    timers = t;
    if (!timer_thread_running) {
        static pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&timer_cond, &ca);
        QemuThread th;
        qemu_thread_create(&th, "nv2a.timer", timer_thread, NULL, QEMU_THREAD_DETACHED);
        timer_thread_running = true;
    }
    pthread_mutex_unlock(&timer_lock);
}

void timer_mod(QEMUTimer *t, int64_t expire_ns)
{
    pthread_mutex_lock(&timer_lock);
    t->expire = expire_ns;
    pthread_cond_broadcast(&timer_cond);
    pthread_mutex_unlock(&timer_lock);
}

void timer_del(QEMUTimer *t)
{
    pthread_mutex_lock(&timer_lock);
    t->expire = -1;
    pthread_mutex_unlock(&timer_lock);
}

bool timer_pending(QEMUTimer *t) { return t->expire >= 0; }

/* ---- dirty memory ------------------------------------------------------- */

/* Without hardware dirty logging, guest RAM written by the CPU can't be
   told apart from unchanged RAM.  Vertex data is always refetched.
   Texture pages count as possibly changed once per displayed frame (or
   on every draw with XBCOMPAT_GPU_TEXCHECK=draw); xemu then compares
   content hashes, so an unchanged texture is not uploaded again.  A
   surface's memory only counts as changed when the GPU side marked it,
   so a render target is never overwritten by stale RAM.  Pages the GPU
   wrote are tracked per client so the VGA client still sees them. */
#define DIRTY_PAGES (128u << 20 >> 12)
static uint8_t dirty_map[DIRTY_PAGES];   /* bit per client */
static uint32_t tex_epoch[DIRTY_PAGES];  /* frame a page was last checked + 1 */
static int tex_check_every_draw = -1;
static volatile uint32_t crtc_start_writes;

bool memory_region_test_and_clear_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size, unsigned client)
{
    (void)mr;
    if (client == DIRTY_MEMORY_XBC_VERTEX) return true;
    if (client == DIRTY_MEMORY_NV2A_TEX) {
        if (tex_check_every_draw < 0) {
            const char *e = getenv("XBCOMPAT_GPU_TEXCHECK");
            tex_check_every_draw = e && !strcmp(e, "draw");
        }
        if (tex_check_every_draw) return true;
    }
    uint32_t epoch = crtc_start_writes + 1;
    bool dirty = false;
    uint64_t end = addr + size;
    for (uint64_t p = addr >> 12; p < DIRTY_PAGES && (p << 12) < end; p++) {
        if (dirty_map[p] & (1u << client)) { dirty = true; dirty_map[p] &= ~(1u << client); }
        if (client == DIRTY_MEMORY_NV2A_TEX && tex_epoch[p] != epoch) { dirty = true; tex_epoch[p] = epoch; }
    }
    return dirty;
}

void memory_region_set_client_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size, unsigned client)
{
    (void)mr;
    uint64_t end = addr + size;
    for (uint64_t p = addr >> 12; p < DIRTY_PAGES && (p << 12) < end; p++) dirty_map[p] |= 1u << client;
}

void memory_region_set_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size)
{
    (void)mr;
    uint64_t end = addr + size;
    for (uint64_t p = addr >> 12; p < DIRTY_PAGES && (p << 12) < end; p++) dirty_map[p] = 0xFF;
}

/* ---- VGA register file -------------------------------------------------- */

uint32_t vga_ioport_read(void *opaque, uint32_t addr)
{
    VGACommonState *s = opaque;
    switch (addr) {
    case VGA_ATT_W: return s->ar_flip_flop ? 0 : s->ar_index;
    case VGA_ATT_R: return (s->ar_index & 0x1f) < 21 ? s->ar[s->ar_index & 0x1f] : 0;
    case VGA_MIS_R: return s->msr;
    case VGA_SEQ_I: return s->sr_index;
    case VGA_SEQ_D: return s->sr[s->sr_index];
    case VGA_GFX_I: return s->gr_index;
    case VGA_GFX_D: return s->gr[s->gr_index];
    case VGA_CRT_IC: case VGA_CRT_IM: return s->cr_index;
    case VGA_CRT_DC: case VGA_CRT_DM: return s->cr[s->cr_index];
    case VGA_IS1_RC: case VGA_IS1_RM:
        s->ar_flip_flop = 0;
        /* toggle vertical retrace and display enable so polls finish */
        s->st01 ^= 0x09;
        return s->st01;
    default: return 0;
    }
}

void vga_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    VGACommonState *s = opaque;
    val &= 0xff;
    switch (addr) {
    case VGA_ATT_W:
        if (!s->ar_flip_flop) s->ar_index = val & 0x3f;
        else if ((s->ar_index & 0x1f) < 21) s->ar[s->ar_index & 0x1f] = val;
        s->ar_flip_flop ^= 1;
        break;
    case VGA_MIS_W: s->msr = val; break;
    case VGA_SEQ_I: s->sr_index = val; break;
    case VGA_SEQ_D: s->sr[s->sr_index] = val; break;
    case VGA_GFX_I: s->gr_index = val; break;
    case VGA_GFX_D: s->gr[s->gr_index] = val; break;
    case VGA_CRT_IC: case VGA_CRT_IM: s->cr_index = val; break;
    case VGA_CRT_DC: case VGA_CRT_DM: s->cr[s->cr_index] = val; break;
    default: break;
    }
}

void vga_common_reset(VGACommonState *s)
{
    int w = s->mode_width, h = s->mode_height;
    uint32_t mb = s->vram_size_mb;
    void *bpp = s->get_bpp, *params = s->get_params, *res = s->get_resolution;
    memset(s->cr, 0, sizeof s->cr);
    memset(s->sr, 0, sizeof s->sr);
    memset(s->gr, 0, sizeof s->gr);
    memset(s->ar, 0, sizeof s->ar);
    s->ar_flip_flop = 0;
    s->cr[NV_PRMCIO_INTERLACE_MODE] = NV_PRMCIO_INTERLACE_MODE_DISABLED;
    s->mode_width = w;
    s->mode_height = h;
    s->vram_size_mb = mb;
    s->get_bpp = bpp;
    s->get_params = params;
    s->get_resolution = res;
}

void nv2a_host_get_resolution(VGACommonState *s, int *pwidth, int *pheight)
{
    if (s->mode_width && s->mode_height) {
        *pwidth = s->mode_width;
        *pheight = s->mode_height;
        return;
    }
    int w = (s->cr[VGA_CRTC_H_DISP] + 1) * 8;
    int h = s->cr[VGA_CRTC_V_DISP_END] | ((s->cr[VGA_CRTC_OVERFLOW] & 0x02) << 7) |
            ((s->cr[VGA_CRTC_OVERFLOW] & 0x40) << 3);
    h++;
    *pwidth = w > 8 ? w : 640;
    *pheight = h > 1 ? h : 480;
}

/* ---- the device --------------------------------------------------------- */

static NV2AState dev;
struct gpu_config gpu_cfg;
bool xbc_gpu_lazy_surfaces = true;

void nv2a_host_set_irq(NV2AState *d, bool level)
{
    if (d->irq_level == level) return;
    d->irq_level = level;
    gpu_irq_changed(level);
}

bool gpu_irq_asserted(void) { return dev.irq_level; }

void gpu_host_init(uint8_t *ram, uint64_t ram_size, uint8_t *ramin, uint64_t ramin_size)
{
    g_nv2a = &dev;
    xbc_gpu_lazy_surfaces = !getenv("XBCOMPAT_GPU_COHERENT");
    bql_lock();   /* nv2a_reset expects to run under it, like QEMU's reset */
    nv2a_init(&dev, ram, ram_size, ramin, ramin_size);
    bql_unlock();
    /* What the boot loader leaves programmed before the kernel and the
       title's Direct3D run: four memory partitions and the memory timing. */
    gpu_mmio_write(0x100200, 0x03070103, 4);   /* NV_PFB_CFG0 */
    gpu_mmio_write(0x100204, 0x11448000, 4);   /* NV_PFB_CFG1 */
}

static const NV2ABlockInfo *find_block(uint32_t off)
{
    for (unsigned i = 0; i < ARRAY_SIZE(blocktable); i++) {
        const NV2ABlockInfo *b = &blocktable[i];
        if (b->name && off >= b->offset && off < b->offset + b->size) return b;
    }
    return NULL;
}

uint32_t gpu_mmio_read(uint32_t off, unsigned size)
{
    const NV2ABlockInfo *b = find_block(off);
    uint64_t v = 0;
    bql_lock();
    if (b) v = b->ops.read(&dev, off - b->offset, size);
    bql_unlock();
    if (gpu_cfg.trace_mmio) gpu_log("nv2a: read  %-8s %#08x -> %#x", b ? b->name : "?", off, (uint32_t)v);
    return (uint32_t)v;
}

uint32_t gpu_crtc_start_writes(void) { return crtc_start_writes; }

void gpu_mmio_write(uint32_t off, uint32_t val, unsigned size)
{
    const NV2ABlockInfo *b = find_block(off);
    if (off == 0x600800) crtc_start_writes++;   /* NV_PCRTC_START: a flip */
    if (gpu_cfg.trace_mmio) gpu_log("nv2a: write %-8s %#08x <- %#x", b ? b->name : "?", off, val);
    bql_lock();
    if (b) b->ops.write(&dev, off - b->offset, val, size);
    bql_unlock();
}

void gpu_set_display_mode(int width, int height, uint32_t pitch, int bpp, uint32_t framebuffer)
{
    bql_lock();
    VGACommonState *s = &dev.vga;
    s->mode_width = width;
    s->mode_height = height;
    uint32_t lo = pitch / 8;
    s->cr[0x13] = lo & 0xff;
    s->cr[0x19] = (s->cr[0x19] & 0x1f) | ((lo >> 3) & 0xe0);
    s->cr[0x25] = (s->cr[0x25] & ~0x20) | ((lo >> 6) & 0x20);
    s->cr[0x28] = (s->cr[0x28] & ~3) | (bpp == 32 ? 3 : 2);
    s->cr[NV_PRMCIO_INTERLACE_MODE] = height == 1080 ? 0 : NV_PRMCIO_INTERLACE_MODE_DISABLED;
    if (framebuffer) dev.pcrtc.start = framebuffer & 0x07FFFFFF;
    bql_unlock();
}

uint32_t gpu_crtc_start(void) { return dev.pcrtc.start; }

/* ---- display ------------------------------------------------------------ */

static SDL_Window *window;
static SDL_GLContext window_gl;
static GLuint quad_prog, quad_vao;

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        gpu_log("nv2a: display shader: %s", log);
    }
    return s;
}

void gpu_display_init(int width, int height, bool fullscreen)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) gpu_fatal("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    Uint32 flags = SDL_WINDOW_OPENGL;
    const char *driver = SDL_GetCurrentVideoDriver();
    if (fullscreen || (driver && !strcasecmp(driver, "kmsdrm"))) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window = SDL_CreateWindow("xbcompat-gpu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (!window) gpu_fatal("SDL_CreateWindow: %s", SDL_GetError());
    window_gl = SDL_GL_CreateContext(window);
    if (!window_gl) gpu_fatal("SDL_GL_CreateContext (OpenGL 4.0 core): %s", SDL_GetError());
    SDL_GL_MakeCurrent(window, window_gl);
    gpu_log("nv2a: OpenGL %s on %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    const char *si = getenv("XBCOMPAT_SWAP_INTERVAL");
    SDL_GL_SetSwapInterval(si && !strcmp(si, "1") ? 1 : 0);

    /* xemu's render and display contexts share with this one. */
    nv2a_context_init();
    SDL_GL_MakeCurrent(window, window_gl);

    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER,
        "#version 400\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "  vec2 pos = vec2((gl_VertexID & 1) * 2.0 - 1.0, (gl_VertexID >> 1) * 2.0 - 1.0);\n"
        "  uv = pos * 0.5 + 0.5;\n"
        "  gl_Position = vec4(pos, 0.0, 1.0);\n"
        "}\n"));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER,
        "#version 400\n"
        "in vec2 uv;\n"
        "uniform sampler2D tex;\n"
        "out vec4 color;\n"
        "void main() { color = vec4(texture(tex, uv).rgb, 1.0); }\n"));
    glLinkProgram(p);
    quad_prog = p;
    glGenVertexArrays(1, &quad_vao);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);
}

void gpu_vblank(void)
{
    bql_lock();
    nv2a_host_vblank(&dev);
    bql_unlock();
}

/* Draws the CRTC's surface (when the GPU has one there); true if it did.
   pixels, when given, receives the window's w x h BGRA picture. */
bool gpu_display_frame(uint8_t *pixels, int *pw, int *ph)
{
    GLuint tex = nv2a_get_framebuffer_surface();
    SDL_GL_MakeCurrent(window, window_gl);
    int w, h;
    SDL_GL_GetDrawableSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClear(GL_COLOR_BUFFER_BIT);
    if (tex) {
        glUseProgram(quad_prog);
        glBindVertexArray(quad_vao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(quad_prog, "tex"), 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    nv2a_release_framebuffer_surface();
    if (pixels && pw && ph) {
        *pw = w;
        *ph = h;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
    SDL_GL_SwapWindow(window);
    return tex != 0;
}

bool gpu_poll_events(void)
{
    SDL_Event e;
    bool quit = false;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) quit = true;
    return quit;
}
