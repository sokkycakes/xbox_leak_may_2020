// The Guide: an overlay over every title, drawn by xbcompat at present time.
// The frame (hudbkgnd) and the menus (MainMenuSignedIn, Options) are the
// Games for Windows - LIVE Guide's own XUI scenes from xlive.dll, converted
// to .xui by tools/guide/mkguide.py and played on the frolickdash XUI
// runtime; this file is their host: a GL backend, a directory of assets, the
// open/close logic and the menu actions (xlive fills its texts from code,
// so they are set here).
//
// Hold Back + Start for half a second (or press the Guide button, or F1) to
// open it.  While it is open the title keeps running, sees idle pads and is
// turned down.  A resumes or picks, B closes, Y goes to the dashboard.
#include "xui.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern "C" {
typedef unsigned long ULONG;
ULONG xbe_title_id(void);
const char *xbe_title_name(void);
unsigned xinput_guide_read(int *port);
void xinput_guide_mask(bool on);
void ds_set_duck(float gain);
void ds_set_volume(float gain);
void ds_play_ui(const float *pcm, int frames);
unsigned xinput_guide_ports(void);
void xinput_guide_vibration(bool on);
const char *fs_hdd_root(void);
void reset_request_always(const char *why);
void xlog(const char *fmt, ...);
bool guide_frame(int w, int h);
void guide_draw(int w, int h);
}

#define GUIDE_UP 0x1
#define GUIDE_DOWN 0x2
#define GUIDE_LEFT 0x4
#define GUIDE_RIGHT 0x8
#define GUIDE_START 0x10
#define GUIDE_BACK 0x20
#define GUIDE_A 0x100
#define GUIDE_B 0x200
#define GUIDE_X 0x400
#define GUIDE_Y 0x800
#define GUIDE_LT 0x1000
#define GUIDE_RT 0x2000
#define GUIDE_HOME 0x4000

namespace {

// ---- assets: one directory of .xui scenes, .xtx images and font.ttf ------

struct DirResources : xui::Resources {
    char dir[512];
    bool read(const char* path, unsigned char** data, int* len) {
        const char* name = path;
        const char* s = strstr(name, "://");
        if (s) name = s + 3;
        const char* b = strrchr(name, '/'); if (b) name = b + 1;
        b = strrchr(name, '\\'); if (b) name = b + 1;
        if (!*name) return false;
        char nm[256]; snprintf(nm, sizeof nm, "%s", name);
        int n = (int)strlen(nm);
        if (n > 4 && !strcasecmp(nm + n - 4, ".png")) strcpy(nm + n - 4, ".xtx");
        char full[800]; snprintf(full, sizeof full, "%s/%s", dir, nm);
        FILE* f = fopen(full, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
        if (l < 0) { fclose(f); return false; }
        *data = new unsigned char[l + 1];
        *len = (int)fread(*data, 1, l, f); (*data)[*len] = 0;
        fclose(f);
        return true;
    }
};

// ---- GL backend: fixed function, client arrays, called inside d3d8.c's
// guide_overlay() bracket (attributes pushed, window framebuffer bound). ----

typedef void (APIENTRY *PFNBLENDSEP)(GLenum, GLenum, GLenum, GLenum);
typedef void (APIENTRY *PFNBLENDEQ)(GLenum);

struct GLBackend : xui::Backend {
    int W, H; float kx;  // window size; extra x stretch (non-square output)
    PFNBLENDSEP blendSep; PFNBLENDEQ blendEq;
    GLBackend() : W(0), H(0), kx(1), blendSep(0), blendEq(0) {}
    xui::Texture upload(const xui::Image& img) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, img.pix);
        return t;
    }
    void clip(const float* r) {
        if (!r) { glDisable(GL_SCISSOR_TEST); return; }
        int x0 = (int)(r[0] * kx), x1 = (int)(r[2] * kx + 0.999f);
        int y0 = (int)r[1], y1 = (int)(r[3] + 0.999f);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0, H - y1, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
    }
    void triangles(xui::Texture tex, xui::Blend blend, const xui::Vertex* v, int nv, const unsigned short* idx, int ni) {
        if (tex) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex); }
        else glDisable(GL_TEXTURE_2D);
        if (blend == xui::BlendAdd) blendSep(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
        else if (blend == xui::BlendMultiply) blendSep(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
        else blendSep(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glVertexPointer(2, GL_FLOAT, sizeof(xui::Vertex), &v[0].x);
        glTexCoordPointer(2, GL_FLOAT, sizeof(xui::Vertex), &v[0].u);
        glColorPointer(4, GL_FLOAT, sizeof(xui::Vertex), &v[0].r);
        glDrawElements(GL_TRIANGLES, ni, GL_UNSIGNED_SHORT, idx);
        (void)nv;
    }
    void begin(int w, int h, float stretch) {
        W = w; H = h; kx = stretch;
        if (!blendSep) {
            blendSep = (PFNBLENDSEP)SDL_GL_GetProcAddress("glBlendFuncSeparate");
            blendEq = (PFNBLENDEQ)SDL_GL_GetProcAddress("glBlendEquation");
        }
        glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_ALPHA_TEST);
        glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glDisable(GL_FOG);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_COLOR_LOGIC_OP); glDisable(GL_POLYGON_STIPPLE);
        for (int i = 0; i < 6; i++) glDisable(GL_CLIP_PLANE0 + i);
        glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T);
        glDisable(GL_TEXTURE_GEN_R); glDisable(GL_TEXTURE_GEN_Q);
        glColorMask(1, 1, 1, 1);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glShadeModel(GL_SMOOTH);
        glEnable(GL_BLEND);
        if (blendEq) blendEq(GL_FUNC_ADD);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, w, h, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glScalef(kx, 1, 1);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_SECONDARY_COLOR_ARRAY);
        glDisableClientState(GL_FOG_COORD_ARRAY);
        glDisableClientState(GL_EDGE_FLAG_ARRAY);
        glDisableClientState(GL_INDEX_ARRAY);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
    }
    void end() {
        glMatrixMode(GL_TEXTURE); glPopMatrix();
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
    }
};

// ---- the Guide ----------------------------------------------------------

enum State { Off, Closed, Opening, Open, Closing, Leaving };

// A Guide sound (the .wav files from xlive), as 48 kHz stereo float.
struct Sound { float* pcm; int frames; Sound() : pcm(0), frames(0) {} };
enum { SndOpen, SndClose, SndFocus, SndSelect, SndBack, SndCount };
const char* const soundFiles[SndCount] = { "HUD_open.wav", "HUD_close.wav", "btn_Focus.wav", "btn_Select.wav", "btn_Back.wav" };

struct Guide {
    DirResources res;
    GLBackend be;
    xui::Renderer* r;
    xui::Element *frameEl, *mainEl, *settingsEl, *skinEl;
    xui::Scene *frame, *main, *settings, *menu;  // menu: the one showing
    xui::Node* hud;  // HUDRootScene: owns the open/close timelines
    State state;
    unsigned prev;
    double holdStart, lastTime, repeatAt, leaveAt;
    int port;
    bool armed;  // Back + Start must be let go once before they open it
    int volume;  // 0..10
    bool vibration;
    Sound sounds[SndCount];
    Guide() : r(0), frameEl(0), mainEl(0), settingsEl(0), skinEl(0), frame(0), main(0), settings(0), menu(0),
              hud(0), state(Off), prev(0), holdStart(-1), lastTime(-1), repeatAt(0), leaveAt(0), port(0),
              armed(false), volume(10), vibration(true) {}
};

Guide g;
bool tried;

double now_s() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void set_text(xui::Scene* s, const char* id, const char* text) {
    xui::Node* n = s->find(id);
    if (!n) return;
    xui::Value& v = n->props.set("Text"); v.free(); v.type = xui::TString; v.s = xui::strdup2(text);
}

void set_show(xui::Scene* s, const char* id, bool on) {
    xui::Node* n = s->find(id);
    if (!n) return;
    xui::Value& v = n->props.set("Show"); v.free(); v.type = xui::TBool; v.b = on;
}

void set_str(xui::Scene* s, const char* id, const char* key, const char* val) {
    xui::Node* n = s->find(id);
    if (!n) return;
    xui::Value& v = n->props.set(key); v.free(); v.type = xui::TString; v.s = xui::strdup2(val);
}

void set_pos(xui::Scene* s, const char* id, float x, float y) {
    xui::Node* n = s->find(id);
    if (!n) return;
    xui::Value& v = n->props.set("Position"); v.free(); v.type = xui::TVector; v.v.x = x; v.v.y = y; v.v.z = 0;
}

// Hide an element whose Show the frame's timelines own: hide what it draws.
void hide_drawn(xui::Scene* s, const char* id) {
    xui::Node* n = s->find(id);
    if (!n) return;
    xui::Node* t = n->visual ? n->visual : n;
    xui::Value& v = t->props.set("Opacity"); v.free(); v.type = xui::TFloat; v.f = 0;
    if (n->visual) return;
    for (int i = 0; i < n->children.n; i++) {
        xui::Value& c = n->children[i]->props.set("Opacity"); c.free(); c.type = xui::TFloat; c.f = 0;
    }
}

void set_image(xui::Scene* s, const char* id, const char* path) {
    xui::Node* n = s->find(id);
    if (!n) return;
    xui::Value& v = n->props.set("ImagePath"); v.free(); v.type = xui::TString; v.s = xui::strdup2(path);
}

// Settings live beside the hard disk partitions, so every title (each its
// own xbcompat process) shares them: <hdd>/.guide, "volume=N vibration=N".
void settings_path(char* out, int n) {
    const char* e = getenv("XBCOMPAT_GUIDE_SETTINGS");
    if (e) snprintf(out, n, "%s", e);
    else snprintf(out, n, "%s/.guide", fs_hdd_root() ? fs_hdd_root() : ".");
}

void apply_settings() {
    float v = g.volume / 10.0f;
    ds_set_volume(v * v);
    xinput_guide_vibration(g.vibration);
}

void read_settings() {
    char path[600]; settings_path(path, sizeof path);
    FILE* f = fopen(path, "r");
    if (f) {
        int v = 10, r = 1;
        if (fscanf(f, "volume=%d vibration=%d", &v, &r) >= 1) {
            g.volume = v < 0 ? 0 : v > 10 ? 10 : v;
            g.vibration = r != 0;
        }
        fclose(f);
    }
    apply_settings();
}

void write_settings() {
    char path[600]; settings_path(path, sizeof path);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "volume=%d vibration=%d\n", g.volume, g.vibration ? 1 : 0);
    fclose(f);
}

void settings_text();
void load_sound(Sound& snd, const char* name);

bool load() {
    tried = true;
    const char* e = getenv("XBCOMPAT_GUIDE");
    if (e && !strcmp(e, "0")) return false;
    ULONG id = xbe_title_id();
    // Not over the dashboard or the boot animation, unless asked for.
    if (!(e && !strcmp(e, "1")) && (id == 0xFFFE0000 || id == 0x00112233)) return false;
    const char* dir = getenv("XBCOMPAT_GUIDE_DIR");
    if (dir) snprintf(g.res.dir, sizeof g.res.dir, "%s", dir);
    else {
        char exe[400]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n <= 0) return false;
        exe[n] = 0;
        char* sl = strrchr(exe, '/'); if (sl) *sl = 0;
        snprintf(g.res.dir, sizeof g.res.dir, "%s/guide", exe);
    }
    char err[256];
    g.skinEl = g.res.scene("hudskin.xui", err, sizeof err);
    if (!g.skinEl) { xlog("Guide: off, no skin in %s (%s)", g.res.dir, err); return false; }
    g.frameEl = g.res.scene("hudbkgnd.xui", err, sizeof err);
    g.mainEl = g.frameEl ? g.res.scene("GuideMain.xui", err, sizeof err) : 0;
    g.settingsEl = g.mainEl ? g.res.scene("GuideSettings.xui", err, sizeof err) : 0;
    if (!g.settingsEl) { xlog("Guide: off, scene load failed: %s", err); return false; }
    g.r = new xui::Renderer(&g.be, &g.res);
    g.r->setFont("font.ttf");
    g.frame = new xui::Scene(g.frameEl, g.skinEl);
    g.frame->setFocus(0);
    g.hud = g.frame->find("HUDRootScene");
    if (!g.hud) g.hud = g.frame->root;
    g.main = g.menu = new xui::Scene(g.mainEl, g.skinEl);
    g.settings = new xui::Scene(g.settingsEl, g.skinEl);
    // Frame legends (the 360 fills them from the menu's hidden legend buttons).
    set_text(g.frame, "Legend_A", "Select");
    set_text(g.frame, "Legend_B", "Back");
    set_text(g.frame, "Legend_Y", "Xbox Dashboard");
    hide_drawn(g.frame, "Legend_X");
    hide_drawn(g.frame, "CloseX");          // the PC Guide's mouse close box
    hide_drawn(g.frame, "EvaluationTextId");
    set_text(g.frame, "GamerTag", "Player");
    // The signed-in main menu: the game's name where the gamercard goes,
    // then Resume Game, Xbox Dashboard and Settings in its button rows.
    char title[64]; snprintf(title, sizeof title, "%s", xbe_title_name());
    set_text(g.main, "txtSignInLive", title[0] ? title : "Xbox Game");
    set_pos(g.main, "txtSignInLive", 146, 128);
    set_show(g.main, "scnCommunity", false);
    set_show(g.main, "btnBlank1", true);
    set_text(g.main, "btnBlank1", "Resume Game");
    set_image(g.main, "btnBlank1", "ico_32x_Controller.png");
    set_text(g.main, "btnQuickChat", "Xbox Dashboard");
    set_image(g.main, "btnQuickChat", "homekey.png");
    set_text(g.main, "btnOptions", "Settings");
    set_str(g.main, "btnBlank1", "NavUp", "");
    set_str(g.main, "btnBlank1", "NavDown", "btnQuickChat");
    set_str(g.main, "btnQuickChat", "NavUp", "btnBlank1");
    // Settings: the Guide's Personal Settings page with our three rows.
    set_text(g.settings, "labelHeading", "Settings");
    set_show(g.settings, "btnNetwork", false);
    set_show(g.settings, "btnSQM", false);
    set_show(g.settings, "btnAddLive", false);
    set_show(g.settings, "btnUpgrade", false);
    set_str(g.settings, "btnNotifications", "NavDown", "");
    read_settings();
    settings_text();
    for (int i = 0; i < SndCount; i++) load_sound(g.sounds[i], soundFiles[i]);
    g.state = Closed;
    xlog("Guide: ready (%s); hold Back + Start, or Guide / F1", g.res.dir);
    return true;
}

void load_sound(Sound& snd, const char* name) {
    unsigned char* data; int len;
    if (!g.res.read(name, &data, &len)) return;
    SDL_AudioSpec spec; Uint8* buf; Uint32 blen;
    // The Guide's sounds are 16-bit PCM, mono or stereo, 44.1 or 48 kHz:
    // to 48 kHz stereo float with a linear resample.
    if (SDL_LoadWAV_RW(SDL_RWFromConstMem(data, len), 1, &spec, &buf, &blen)) {
        int ch = spec.channels;
        if (spec.format == AUDIO_S16LSB && (ch == 1 || ch == 2) && spec.freq > 0) {
            const Sint16* in = (const Sint16*)buf;
            int inFrames = (int)(blen / (2 * ch));
            int frames = (int)((long long)inFrames * 48000 / spec.freq);
            float* out = (float*)malloc(sizeof(float) * 2 * (frames + 1));
            double step = spec.freq / 48000.0;
            for (int i = 0; i < frames; i++) {
                double p = i * step; int k = (int)p; float f = (float)(p - k);
                int k1 = k + 1 < inFrames ? k + 1 : k;
                for (int c = 0; c < 2; c++) {
                    int cc = ch == 2 ? c : 0;
                    float x0 = in[k * ch + cc], x1 = in[k1 * ch + cc];
                    out[2 * i + c] = (x0 + (x1 - x0) * f) / 32768.0f;
                }
            }
            snd.pcm = out; snd.frames = frames;
        }
        SDL_FreeWAV(buf);
    }
    delete[] data;
}

void play(int which) {
    if (g.sounds[which].pcm) ds_play_ui(g.sounds[which].pcm, g.sounds[which].frames);
}

void update_clock() {
    time_t t = time(0); struct tm lt; localtime_r(&t, &lt);
    int h = lt.tm_hour % 12; if (!h) h = 12;
    char buf[16]; snprintf(buf, sizeof buf, "%d:%02d%s", h, lt.tm_min, lt.tm_hour < 12 ? "am" : "pm");
    set_text(g.frame, "DateTimeTextId", buf);
}

void show_port() {
    xui::Node* rol = g.frame->find("ROL");
    if (!rol || !rol->visual) return;
    for (int i = 0; i < 4; i++) {
        char id[8]; snprintf(id, sizeof id, "light%d", i + 1);
        xui::Node* l = rol->visual->find(id);
        if (!l) continue;
        xui::Value& v = l->props.set("Show"); v.free(); v.type = xui::TBool; v.b = i == g.port;
    }
}

void settings_text() {
    char buf[64];
    snprintf(buf, sizeof buf, "Volume   %d%%", g.volume * 10);
    set_text(g.settings, "btnOnlineStatus", buf);
    snprintf(buf, sizeof buf, "Vibration   %s", g.vibration ? "On" : "Off");
    set_text(g.settings, "btnVoice", buf);
    unsigned ports = xinput_guide_ports();
    int k = snprintf(buf, sizeof buf, "Controllers  ");
    for (int i = 0; i < 4; i++)
        if (ports & (1u << i)) k += snprintf(buf + k, sizeof buf - k, " %d", i + 1);
    if (!ports) snprintf(buf + k, sizeof buf - k, " none");
    set_text(g.settings, "btnNotifications", buf);
}

void show_menu(xui::Scene* m, const char* focus) {
    g.menu = m;
    m->setFocus(m->find(focus));
}

void open_guide() {
    g.state = Opening;
    g.hud->play("ClosedToHalf");
    show_menu(g.main, "btnBlank1");
    update_clock();
    show_port();
    xinput_guide_mask(true);
    ds_set_duck(0.3f);
    play(SndOpen);
    xlog("Guide: open (pad %d)", g.port + 1);
}

void close_guide() {
    g.state = Closing;
    g.hud->play("HalfToClosed");
    play(SndClose);
    xinput_guide_mask(false);
    ds_set_duck(1.0f);
}

void press(const char* id) {
    if (!strcmp(id, "btnBlank1")) close_guide();
    else if (!strcmp(id, "btnOptions")) { settings_text(); show_menu(g.settings, "btnOnlineStatus"); }
    else if (!strcmp(id, "btnVoice")) { g.vibration = !g.vibration; apply_settings(); write_settings(); settings_text(); }
    else if (!strcmp(id, "btnQuickChat")) {
        g.state = Leaving;
        g.leaveAt = now_s() + 0.15;  // let the audio fade first
        ds_set_duck(0.0f);
    }
}

void step(double t) {
    int port = g.port;
    unsigned b = xinput_guide_read(&port);
    unsigned down = b & ~g.prev;
    g.prev = b;
    bool combo = (b & (GUIDE_BACK | GUIDE_START)) == (GUIDE_BACK | GUIDE_START) && !(b & (GUIDE_LT | GUIDE_RT));
    if (!combo) { g.armed = true; g.holdStart = -1; }
    bool toggle = (down & GUIDE_HOME) != 0;
    if (combo && g.armed) {
        if (g.holdStart < 0) g.holdStart = t;
        else if (t - g.holdStart >= 0.5) { toggle = true; g.armed = false; }
    }
    switch (g.state) {
    case Closed:
        if (toggle) { g.port = port; open_guide(); }
        break;
    case Opening:
    case Open:
        if (g.state == Opening && !g.hud->playing) g.state = Open;
        if ((down & GUIDE_B) && g.menu == g.settings) { play(SndBack); show_menu(g.main, "btnOptions"); break; }
        if (toggle || (down & GUIDE_B)) { close_guide(); break; }
        if ((down & (GUIDE_LEFT | GUIDE_RIGHT)) && g.menu == g.settings && g.menu->focus &&
            !strcmp(g.menu->focus->id(), "btnOnlineStatus")) {
            g.volume += (down & GUIDE_RIGHT) ? 1 : -1;
            g.volume = g.volume < 0 ? 0 : g.volume > 10 ? 10 : g.volume;
            apply_settings(); write_settings(); settings_text();
            play(SndFocus);
            break;
        }
        if (down & GUIDE_Y) { press("btnQuickChat"); break; }
        if (down & GUIDE_A) {
            if (g.menu->focus) {
                const char* id = g.menu->focus->id();
                if (strcmp(id, "btnBlank1")) play(SndSelect);  // Resume plays the close sound
                g.menu->press();
                press(id);
            }
            break;
        }
        if (b & (GUIDE_UP | GUIDE_DOWN)) {
            if ((down & (GUIDE_UP | GUIDE_DOWN)) || t >= g.repeatAt) {
                if (g.menu->navigate((b & GUIDE_UP) ? "up" : "down")) play(SndFocus);
                g.repeatAt = t + ((down & (GUIDE_UP | GUIDE_DOWN)) ? 0.4 : 0.12);
            }
        }
        break;
    case Closing:
        if (!g.hud->playing) g.state = Closed;
        break;
    case Leaving:
        if (t >= g.leaveAt) reset_request_always("Guide");
        break;
    case Off:
        break;
    }
}

}  // namespace

// Once a frame from swap_present: input and animation; true when there is
// something to draw.
bool guide_frame(int w, int h) {
    if (!tried && !load()) return false;
    if (g.state == Off) return false;
    double t = now_s();
    double dt = g.lastTime < 0 ? 1.0 / 60 : t - g.lastTime;
    if (dt > 0.1) dt = 0.1;
    g.lastTime = t;
    step(t);
    if (g.state == Closed) return false;
    g.frame->update(dt);
    g.menu->update(dt);
    if (g.menu == g.settings && g.state == Open) {
        static double next;
        if (t >= next) { settings_text(); next = t + 1; }  // pads come and go
    }
    (void)w; (void)h;
    return true;
}

void guide_draw(int w, int h) {
    // The frame is a 4:3 canvas (800x600); fit the height and stretch x to
    // the window the way the game image is stretched.
    float cw = g.frame->root->width(), ch = g.frame->root->height();
    float s = h / ch;
    float kx = (w / cw) / s;
    g.be.begin(w, h, kx);
    g.r->draw(g.frame, 0, 0, s);
    // The menu sits in the frame's AppHostElementId and fades with it.
    xui::Node* host = g.frame->find("AppHostElementId");
    float hx = 0, hy = 0, op = 1; bool shown = true;
    for (xui::Node* p = host; p; p = p->parent) {
        xui::Vec3 z = {0, 0, 0};
        xui::Vec3 v = xui::propV(p->props, "Position", z);
        hx += v.x; hy += v.y;
        shown = shown && xui::propB(p->props, "Show", true);
        op *= xui::propF(p->props, "Opacity", 1);
    }
    if (host && shown && op > 0.001f) {
        xui::Value& o = g.menu->root->props.set("Opacity"); o.free(); o.type = xui::TFloat; o.f = op;
        g.r->draw(g.menu, hx * s, hy * s, s);
    }
    g.be.end();
    // Test aid: XBCOMPAT_GUIDE_SHOT=file.ppm saves the window (game and
    // Guide) after the Guide's Nth drawn frame (XBCOMPAT_GUIDE_SHOT_AT, 40).
    static int drawn;
    const char* shot = getenv("XBCOMPAT_GUIDE_SHOT");
    const char* at = getenv("XBCOMPAT_GUIDE_SHOT_AT");
    if (shot && ++drawn == (at ? atoi(at) : 40)) {
        unsigned char* px = new unsigned char[w * h * 3];
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
        FILE* f = fopen(shot, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; y--) fwrite(px + y * w * 3, 1, w * 3, f);
            fclose(f);
        }
        delete[] px;
    }
}

// No C++ library is linked (the Sion bundle has none for i386): the few
// runtime pieces the XUI code needs.
#include <stdlib.h>
void* operator new(size_t n) { return malloc(n ? n : 1); }
void* operator new[](size_t n) { return malloc(n ? n : 1); }
void operator delete(void* p) { free(p); }
void operator delete[](void* p) { free(p); }
extern "C" void __cxa_pure_virtual(void) { abort(); }
