// kazeta-guide: xbcompat's Guide (src/guide/guide.cpp on the frolickdash XUI
// runtime) for a cart that isn't an Xbox game, drawn as a gamescope overlay
// window over the title.
//
// It owns the real pads (SDL, HIDAPI over libusb, as xbcompat does) and hands
// the title a uinput Xbox 360 pad with the same state, idle while the Guide is
// open, the way xbcompat masks a title's XInput.  Y in the Guide ("Xbox
// Dashboard") exits with status 42; the cart runner then ends the title.
//
// Environment: DISPLAY (gamescope's Xwayland), XBCOMPAT_GUIDE_DIR (the Guide's
// scenes), XBCOMPAT_HDD (for the shared <hdd>/.guide settings), KAZETA_TITLE_ID
// (hex), KAZETA_TITLE_NAME and KAZETA_TITLE_ICON (the cart's PNG icon, for the
// game box and Now Playing), KAZETA_VOLUME_CMD (run with a linear gain, 0 to
// 1, to set the title's volume), KAZETA_SOUND_PCM (ALSA PCM for the Guide's
// sounds), KAZETA_NO_UINPUT=1 to leave the pads to the title.
#define SDL_VIDEO_DRIVER_X11 1
#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_opengl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

// PNG only, for the cart's icon (stb_image, public domain / MIT).
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

extern "C" {
typedef unsigned long ULONG;
bool guide_frame(int w, int h);
void guide_draw(int w, int h);
}

static FILE *logf_ = stderr;
static volatile sig_atomic_t quit_ = 0;
static int leave_ = 0;

extern "C" void xlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(logf_, "kazeta-guide: ");
    vfprintf(logf_, fmt, ap);
    fputc('\n', logf_);
    fflush(logf_);
    va_end(ap);
}

// ---- title ---------------------------------------------------------------

extern "C" ULONG xbe_title_id(void)
{
    const char *e = getenv("KAZETA_TITLE_ID");
    return e ? (ULONG)strtoul(e, 0, 16) : 0x4B5A0000;
}

extern "C" const char *xbe_title_name(void)
{
    const char *e = getenv("KAZETA_TITLE_NAME");
    return e ? e : "";
}

// The cart's icon, 128x128 BGRA as an Xbox title image: scaled to fit,
// centred on transparent (the same picture xbcompat puts in the stub XBE).
extern "C" bool kazeta_title_image(unsigned char **bgra, int *w, int *h)
{
    const char *png = getenv("KAZETA_TITLE_ICON");
    int iw, ih, n;
    unsigned char *rgba = png && *png ? stbi_load(png, &iw, &ih, &n, 4) : 0;
    if (!rgba) {
        if (png && *png) xlog("icon %s: %s", png, stbi_failure_reason());
        return false;
    }
    enum { S = 128 };
    unsigned char *out = (unsigned char *)calloc(S * S, 4);
    float s = (float)S / (iw > ih ? iw : ih);
    float ox = (S - iw * s) / 2, oy = (S - ih * s) / 2;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            unsigned acc[4] = { 0, 0, 0, 0 };
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int px = (int)((x + (sx + 0.5f) / 4 - ox) / s), py = (int)((y + (sy + 0.5f) / 4 - oy) / s);
                    if (px < 0 || py < 0 || px >= iw || py >= ih) continue;
                    const unsigned char *p = rgba + ((size_t)py * iw + px) * 4;
                    acc[0] += p[2] * p[3]; acc[1] += p[1] * p[3]; acc[2] += p[0] * p[3]; acc[3] += p[3];
                }
            unsigned char *d = out + (y * S + x) * 4;
            if (acc[3]) for (int k = 0; k < 3; k++) d[k] = (unsigned char)(acc[k] / acc[3]);
            d[3] = (unsigned char)(acc[3] / 16);
        }
    stbi_image_free(rgba);
    *bgra = out; *w = S; *h = S;
    return true;
}

extern "C" const char *fs_hdd_root(void)
{
    const char *e = getenv("XBCOMPAT_HDD");
    return e ? e : "/data/xbox/hdd";
}

extern "C" void reset_request_always(const char *why)
{
    xlog("leaving to the dashboard (%s)", why);
    leave_ = 1;
}

// ---- sound -----------------------------------------------------------------
// The Guide's own sounds go straight to ALSA (libasound, opened by name), not
// through SDL: SDL may ignore AUDIODEV and open "default", which is the
// title's path, turned down with it while the Guide is open.  They play at
// the user's volume, not ducked, as xbcompat mixes them over a game.
// KAZETA_SOUND_PCM names the PCM (default wincart_ui).

static volatile float volume_ = 1, duck_ = 1;
static const float *volatile ui_next_;
static volatile int ui_next_frames_;
static volatile unsigned ui_seq_;

static void apply_volume()
{
    const char *cmd = getenv("KAZETA_VOLUME_CMD");
    if (!cmd || !*cmd) return;
    // The command gets a linear gain, 0 to 1; it turns that into decibels.
    char buf[512];
    snprintf(buf, sizeof buf, "%s %.4f >/dev/null 2>&1 &", cmd, (double)(volume_ * duck_));
    if (system(buf)) {}
}

extern "C" void ds_set_volume(float gain) { volume_ = gain; apply_volume(); }
extern "C" void ds_set_duck(float gain) { duck_ = gain; apply_volume(); }

extern "C" void ds_play_ui(const float *pcm, int frames)
{
    ui_next_ = pcm;
    ui_next_frames_ = frames;
    __atomic_add_fetch(&ui_seq_, 1, __ATOMIC_RELEASE);
}

typedef struct _snd_pcm snd_pcm_t;
static int (*pcm_open_)(snd_pcm_t **, const char *, int, int);
static int (*pcm_set_params_)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
static long (*pcm_writei_)(snd_pcm_t *, const void *, unsigned long);
static int (*pcm_recover_)(snd_pcm_t *, int, int);
static const char *(*strerror_)(int);

static void *sound_thread(void *arg)
{
    snd_pcm_t *pcm = (snd_pcm_t *)arg;
    enum { N = 480 };  // 10 ms
    static float out[N * 2];
    unsigned seen = 0;
    const float *cur = 0;
    int pos = 0, frames = 0;
    while (!quit_) {
        unsigned q = __atomic_load_n(&ui_seq_, __ATOMIC_ACQUIRE);
        if (q != seen) { seen = q; cur = ui_next_; frames = ui_next_frames_; pos = 0; }
        float v = volume_;
        for (int i = 0; i < N; i++) {
            if (cur && pos < frames) {
                out[2 * i] = cur[2 * pos] * v;
                out[2 * i + 1] = cur[2 * pos + 1] * v;
                pos++;
            } else out[2 * i] = out[2 * i + 1] = 0;
        }
        if (cur && pos >= frames) cur = 0;
        long w = pcm_writei_(pcm, out, N);
        if (w < 0 && pcm_recover_(pcm, (int)w, 1) < 0) {
            xlog("Guide sound: %s", strerror_((int)w));
            usleep(100000);
        }
    }
    return 0;
}

static void sound_open()
{
    void *a = dlopen("libasound.so.2", RTLD_NOW);
    if (!a) { xlog("no Guide sounds: %s", dlerror()); return; }
    pcm_open_ = (int (*)(snd_pcm_t **, const char *, int, int))dlsym(a, "snd_pcm_open");
    pcm_set_params_ = (int (*)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned))dlsym(a, "snd_pcm_set_params");
    pcm_writei_ = (long (*)(snd_pcm_t *, const void *, unsigned long))dlsym(a, "snd_pcm_writei");
    pcm_recover_ = (int (*)(snd_pcm_t *, int, int))dlsym(a, "snd_pcm_recover");
    strerror_ = (const char *(*)(int))dlsym(a, "snd_strerror");
    if (!pcm_open_ || !pcm_set_params_ || !pcm_writei_ || !pcm_recover_ || !strerror_) { xlog("no Guide sounds: old libasound"); return; }
    const char *name = getenv("KAZETA_SOUND_PCM");
    if (!name || !*name) name = "wincart_ui";
    snd_pcm_t *pcm = 0;
    // SND_PCM_STREAM_PLAYBACK 0, SND_PCM_FORMAT_FLOAT_LE 14, SND_PCM_ACCESS_RW_INTERLEAVED 3
    int e = pcm_open_(&pcm, name, 0, 0);
    if (e >= 0) e = pcm_set_params_(pcm, 14, 3, 2, 48000, 1, 40000);
    if (e < 0) { xlog("no Guide sounds: %s: %s", name, strerror_(e)); return; }
    pthread_t t;
    pthread_create(&t, 0, sound_thread, pcm);
    pthread_detach(t);
    xlog("Guide sounds on %s", name);
}

// ---- pads --------------------------------------------------------------------

#define GUIDE_A 0x100
#define GUIDE_B 0x200
#define GUIDE_X 0x400
#define GUIDE_Y 0x800
#define GUIDE_LT 0x1000
#define GUIDE_RT 0x2000
#define GUIDE_HOME 0x4000

enum { PORTS = 4 };
static SDL_GameController *pads_[PORTS];
static SDL_JoystickID ids_[PORTS];
static bool masked_, vibration_ = true;
static int ui_ = -1;

static char ui_node_[64];  // our uinput pad's /dev/input/eventN

static bool is_virtual(int index)
{
    const char *n = SDL_JoystickNameForIndex(index);
    if (n && strstr(n, "Kazeta")) return true;
    const char *p = SDL_JoystickPathForIndex(index);
    return p && ui_node_[0] && !strcmp(p, ui_node_);
}

static void sync_pads()
{
    for (int i = 0; i < PORTS; i++)
        if (pads_[i] && !SDL_GameControllerGetAttached(pads_[i])) {
            xlog("pad %d gone", i + 1);
            SDL_GameControllerClose(pads_[i]);
            pads_[i] = 0;
        }
    for (int j = 0; j < SDL_NumJoysticks(); j++) {
        if (!SDL_IsGameController(j) || is_virtual(j)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        bool have = false;
        for (int i = 0; i < PORTS; i++) have |= pads_[i] && ids_[i] == id;
        if (have) continue;
        for (int i = 0; i < PORTS; i++)
            if (!pads_[i]) {
                pads_[i] = SDL_GameControllerOpen(j);
                ids_[i] = id;
                if (pads_[i]) xlog("pad %d: %s", i + 1, SDL_GameControllerName(pads_[i]));
                break;
            }
    }
}

static unsigned bits(SDL_GameController *c)
{
    unsigned b = 0;
    struct { SDL_GameControllerButton s; unsigned g; } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x1 }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x2 },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x4 }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x8 },
        { SDL_CONTROLLER_BUTTON_START, 0x10 }, { SDL_CONTROLLER_BUTTON_BACK, 0x20 },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x40 }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x80 },
        { SDL_CONTROLLER_BUTTON_A, GUIDE_A }, { SDL_CONTROLLER_BUTTON_B, GUIDE_B },
        { SDL_CONTROLLER_BUTTON_X, GUIDE_X }, { SDL_CONTROLLER_BUTTON_Y, GUIDE_Y },
        { SDL_CONTROLLER_BUTTON_GUIDE, GUIDE_HOME },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GameControllerGetButton(c, map[i].s)) b |= map[i].g;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) b |= GUIDE_LT;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) b |= GUIDE_RT;
    int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (ly < -16000) b |= 0x1;
    if (ly > 16000) b |= 0x2;
    if (lx < -16000) b |= 0x4;
    if (lx > 16000) b |= 0x8;
    return b;
}

// KAZETA_GUIDE_SCRIPT="SECONDS:MASK,...": Guide buttons pressed as if on a
// pad, MASK (hex, the GUIDE_* bits) from SECONDS after start, for tests.
static unsigned script_buttons()
{
    static const char *s = getenv("KAZETA_GUIDE_SCRIPT");
    static Uint32 t0 = SDL_GetTicks();
    if (!s || !*s) return 0;
    double t = (SDL_GetTicks() - t0) / 1000.0;
    unsigned m = 0;
    for (const char *p = s; p && *p;) {
        char *e;
        double at = strtod(p, &e);
        if (*e != ':') break;
        unsigned v = (unsigned)strtoul(e + 1, &e, 16);
        if (at > t) break;
        m = v;
        p = *e == ',' ? e + 1 : 0;
    }
    return m;
}

extern "C" unsigned xinput_guide_read(int *port)
{
    static unsigned last[PORTS];
    unsigned all = 0;
    for (int i = 0; i < PORTS; i++) {
        unsigned b = pads_[i] ? bits(pads_[i]) : 0;
        if (port && (b & ~last[i])) *port = i;
        last[i] = b;
        all |= b;
    }
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    if (k && k[SDL_SCANCODE_F1]) all |= GUIDE_HOME;
    return all | script_buttons();
}

extern "C" unsigned xinput_guide_ports(void)
{
    unsigned m = 0;
    for (int i = 0; i < PORTS; i++) if (pads_[i]) m |= 1u << i;
    return m;
}

extern "C" void xinput_guide_mask(bool on)
{
    masked_ = on;
    if (on)
        for (int i = 0; i < PORTS; i++) if (pads_[i]) SDL_GameControllerRumble(pads_[i], 0, 0, 0);
}

extern "C" void xinput_guide_vibration(bool on) { vibration_ = on; }

// The title's pad: an Xbox 360 pad on uinput, laid out as the kernel's xpad
// driver reports one, so SDL and Wine map it like a real one.
static void ui_abs(int code, int min, int max, int fuzz, int flat)
{
    struct uinput_abs_setup a;
    memset(&a, 0, sizeof a);
    a.code = code;
    a.absinfo.minimum = min;
    a.absinfo.maximum = max;
    a.absinfo.fuzz = fuzz;
    a.absinfo.flat = flat;
    ioctl(ui_, UI_ABS_SETUP, &a);
}

static const int ui_buttons[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };

static void ui_open()
{
    if (getenv("KAZETA_NO_UINPUT")) return;
    ui_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ui_ < 0) { xlog("no /dev/uinput: the title reads the pads itself"); return; }
    ioctl(ui_, UI_SET_EVBIT, EV_KEY);
    ioctl(ui_, UI_SET_EVBIT, EV_ABS);
    ioctl(ui_, UI_SET_EVBIT, EV_FF);
    for (size_t i = 0; i < sizeof ui_buttons / sizeof ui_buttons[0]; i++) ioctl(ui_, UI_SET_KEYBIT, ui_buttons[i]);
    int axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y };
    for (size_t i = 0; i < sizeof axes / sizeof axes[0]; i++) ioctl(ui_, UI_SET_ABSBIT, axes[i]);
    ioctl(ui_, UI_SET_FFBIT, FF_RUMBLE);
    ui_abs(ABS_X, -32768, 32767, 16, 128);
    ui_abs(ABS_Y, -32768, 32767, 16, 128);
    ui_abs(ABS_RX, -32768, 32767, 16, 128);
    ui_abs(ABS_RY, -32768, 32767, 16, 128);
    ui_abs(ABS_Z, 0, 255, 0, 0);
    ui_abs(ABS_RZ, 0, 255, 0, 0);
    ui_abs(ABS_HAT0X, -1, 1, 0, 0);
    ui_abs(ABS_HAT0Y, -1, 1, 0, 0);
    struct uinput_setup s;
    memset(&s, 0, sizeof s);
    s.id.bustype = BUS_USB;
    s.id.vendor = 0x045e;
    s.id.product = 0x028e;
    s.id.version = 0x0114;
    snprintf(s.name, sizeof s.name, "Kazeta X-Box 360 pad");
    s.ff_effects_max = 16;
    if (ioctl(ui_, UI_DEV_SETUP, &s) || ioctl(ui_, UI_DEV_CREATE)) {
        xlog("uinput setup failed");
        close(ui_);
        ui_ = -1;
        return;
    }
    char sys[64] = "";
    if (ioctl(ui_, UI_GET_SYSNAME(sizeof sys), sys) >= 0) {
        char dir[160];
        snprintf(dir, sizeof dir, "/sys/devices/virtual/input/%s", sys);
        for (int tries = 0; tries < 50 && !ui_node_[0]; tries++) {
            for (int e = 0; e < 64; e++) {
                char ev[200];
                snprintf(ev, sizeof ev, "%s/event%d", dir, e);
                if (access(ev, F_OK) == 0) { snprintf(ui_node_, sizeof ui_node_, "/dev/input/event%d", e); break; }
            }
            if (!ui_node_[0]) usleep(20000);
        }
    }
    xlog("virtual pad created (%s)", ui_node_[0] ? ui_node_ : "node unknown");
}

static void ui_emit(int type, int code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.code = code;
    e.value = value;
    if (write(ui_, &e, sizeof e) < 0) {}
}

// Rumble the title asks for goes to the first real pad.
static struct ff_effect effects_[16];

static void ui_service()
{
    struct input_event e;
    while (read(ui_, &e, sizeof e) == (ssize_t)sizeof e) {
        if (e.type == EV_UINPUT && e.code == UI_FF_UPLOAD) {
            struct uinput_ff_upload up;
            memset(&up, 0, sizeof up);
            up.request_id = e.value;
            if (!ioctl(ui_, UI_BEGIN_FF_UPLOAD, &up)) {
                if (up.effect.id >= 0 && up.effect.id < 16) effects_[up.effect.id] = up.effect;
                up.retval = 0;
                ioctl(ui_, UI_END_FF_UPLOAD, &up);
            }
        } else if (e.type == EV_UINPUT && e.code == UI_FF_ERASE) {
            struct uinput_ff_erase er;
            memset(&er, 0, sizeof er);
            er.request_id = e.value;
            if (!ioctl(ui_, UI_BEGIN_FF_ERASE, &er)) { er.retval = 0; ioctl(ui_, UI_END_FF_ERASE, &er); }
        } else if (e.type == EV_FF && e.code < 16) {
            const struct ff_effect &f = effects_[e.code];
            for (int p = 0; p < PORTS; p++) {
                SDL_GameController *c = pads_[p];
                if (!c || masked_ || !vibration_ || f.type != FF_RUMBLE) continue;
                Uint32 ms = f.replay.length ? f.replay.length : 1000;
                if (e.value) SDL_GameControllerRumble(c, f.u.rumble.strong_magnitude, f.u.rumble.weak_magnitude, ms);
                else SDL_GameControllerRumble(c, 0, 0, 0);
            }
        }
    }
}

static void ui_forward()
{
    if (ui_ < 0) return;
    // Every real pad drives the title's pad (buttons pressed on any, the
    // stick pushed furthest), like one controller in port 1.
    static int last[32];
    static bool init;
    if (!init) { for (int i = 0; i < 32; i++) last[i] = 0x7fffffff; init = true; }
    int v[19] = { 0 };
    static const SDL_GameControllerButton sb[11] = { SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
                                                     SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_BACK,
                                                     SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_LEFTSTICK,
                                                     SDL_CONTROLLER_BUTTON_RIGHTSTICK };
    static const SDL_GameControllerAxis ax[6] = { SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_AXIS_RIGHTX,
                                                  SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT };
    for (int p = 0; p < PORTS && !masked_; p++) {
        SDL_GameController *c = pads_[p];
        if (!c) continue;
        for (int i = 0; i < 11; i++) v[i] |= SDL_GameControllerGetButton(c, sb[i]);
        for (int i = 0; i < 6; i++) {
            int a = SDL_GameControllerGetAxis(c, ax[i]);
            if (i >= 4) a = a * 255 / 32767;
            if (abs(a) > abs(v[11 + i])) v[11 + i] = a;
        }
        int hx = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) - SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        int hy = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) - SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP);
        if (hx) v[17] = hx;
        if (hy) v[18] = hy;
    }
    v[8] = 0;  // the Guide button is the Guide's
    static const int codes[19] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR,
                                   ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y };
    bool any = false;
    for (int i = 0; i < 19; i++) {
        if (v[i] == last[i]) continue;
        last[i] = v[i];
        ui_emit(i < 11 ? EV_KEY : EV_ABS, codes[i], v[i]);
        any = true;
    }
    if (any) ui_emit(EV_SYN, SYN_REPORT, 0);
}

// ---- window ------------------------------------------------------------------

// A 32-bit (ARGB) GLX visual, so gamescope composites the overlay with its alpha.
static void pick_argb_visual()
{
    Display *d = XOpenDisplay(NULL);
    if (!d) return;
    static const int attrs[] = { GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_DOUBLEBUFFER, True,
                                 GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None };
    int n = 0;
    GLXFBConfig *cfg = glXChooseFBConfig(d, DefaultScreen(d), attrs, &n);
    for (int i = 0; cfg && i < n; i++) {
        XVisualInfo *vi = glXGetVisualFromFBConfig(d, cfg[i]);
        if (!vi) continue;
        XRenderPictFormat *pf = XRenderFindVisualFormat(d, vi->visual);
        bool argb = pf && pf->direct.alphaMask > 0 && vi->depth == 32;
        if (argb) {
            char hex[32];
            snprintf(hex, sizeof hex, "0x%lx", (unsigned long)vi->visualid);
            SDL_SetHint(SDL_HINT_VIDEO_X11_WINDOW_VISUALID, hex);
            xlog("ARGB visual %s", hex);
            XFree(vi);
            break;
        }
        XFree(vi);
    }
    if (cfg) XFree(cfg);
    XCloseDisplay(d);
}

static void mark_overlay(SDL_Window *w)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_X11) return;
    Display *d = info.info.x11.display;
    Window x = info.info.x11.window;
    unsigned long one = 1;
    XChangeProperty(d, x, XInternAtom(d, "GAMESCOPE_EXTERNAL_OVERLAY", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
    XFlush(d);
}

static void on_signal(int) { quit_ = 1; }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    setvbuf(stderr, 0, _IOLBF, 0);

    SDL_SetHint("SDL_HIDAPI_LIBUSB", "1");
    SDL_SetHint("SDL_HIDAPI_LIBUSB_WHITELIST", "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    pick_argb_visual();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        xlog("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    const char *db = getenv("SDL_GAMECONTROLLERCONFIG_FILE");
    if (db) SDL_GameControllerAddMappingsFromFile(db);
    ui_open();

    sound_open();
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    int W = 1024, H = 768;
    if (getenv("KAZETA_GUIDE_W")) W = atoi(getenv("KAZETA_GUIDE_W"));
    if (getenv("KAZETA_GUIDE_H")) H = atoi(getenv("KAZETA_GUIDE_H"));
    // The GL context lives on a window that is never mapped (gamescope never
    // sees it).  The overlay window is created when the Guide opens and
    // destroyed when it closes: gamescope keeps an unmapped overlay, and while
    // it has one it composites the game, which shows blocky artifacts on the
    // HD 610.  KAZETA_GUIDE_MAPPED=1 keeps one overlay mapped all the time,
    // as the first version did.
    const Uint32 wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_SKIP_TASKBAR;
    SDL_Window *home = SDL_CreateWindow("Guide GL", 0, 0, 16, 16, wflags | SDL_WINDOW_HIDDEN);
    if (!home) { xlog("window: %s", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(home);
    if (!ctx) { xlog("GL: %s", SDL_GetError()); return 1; }
    xlog("up: %dx%d, title %08lX %s", W, H, xbe_title_id(), xbe_title_name());

    bool always = getenv("KAZETA_GUIDE_MAPPED") && atoi(getenv("KAZETA_GUIDE_MAPPED"));
    SDL_Window *win = 0;
    bool shown = false;
    auto open_overlay = [&]() -> bool {
        win = SDL_CreateWindow("Guide", 0, 0, W, H, wflags | SDL_WINDOW_HIDDEN);
        if (!win) { xlog("overlay window: %s", SDL_GetError()); return false; }
        mark_overlay(win);
        SDL_ShowWindow(win);
        if (SDL_GL_MakeCurrent(win, ctx)) { xlog("overlay GL: %s", SDL_GetError()); SDL_DestroyWindow(win); win = 0; return false; }
        SDL_GL_SetSwapInterval(1);
        return true;
    };
    auto close_overlay = [&]() {
        for (int i = 0; i < 2; i++) {
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            SDL_GL_SwapWindow(win);
        }
        glFinish();
        SDL_GL_MakeCurrent(home, ctx);
        SDL_DestroyWindow(win);
        win = 0;
        shown = false;
    };
    if (always) open_overlay();
    else SDL_GL_MakeCurrent(home, ctx);
    while (!quit_ && !leave_) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit_ = 1;
        }
        sync_pads();
        if (ui_ >= 0) ui_service();
        bool draw = guide_frame(W, H);
        ui_forward();
        if (draw && !win && !open_overlay()) { SDL_Delay(16); continue; }
        if (!draw && win && !always) close_overlay();
        if (!win) { SDL_Delay(16); continue; }
        int fw, fh;
        SDL_GL_GetDrawableSize(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        if (draw) {
            glPushAttrib(GL_ALL_ATTRIB_BITS);
            guide_draw(fw, fh);
            glPopAttrib();
        }
        if (draw || shown) SDL_GL_SwapWindow(win);
        else SDL_Delay(16);
        shown = draw;
    }
    if (ui_ >= 0) { ioctl(ui_, UI_DEV_DESTROY); close(ui_); }
    SDL_Quit();
    return leave_ ? 42 : 0;
}
