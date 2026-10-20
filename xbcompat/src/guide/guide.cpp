// The Guide: a 360-style overlay over every title, drawn by xbcompat at
// present time.  The frame (xam hudbkgnd) and the menu (hud MainMenu) are
// the 360's own XUI scenes, converted to .xui by tools/guide/mkguide.sh and
// played on the frolickdash XUI runtime; this file is their host: a GL
// backend, a directory of assets, the open/close logic and the menu actions.
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

struct Guide {
    DirResources res;
    GLBackend be;
    xui::Renderer* r;
    xui::Element *frameEl, *menuEl, *skinEl;
    xui::Scene *frame, *menu;
    xui::Node* hud;  // HUDRootScene: owns the open/close timelines
    State state;
    unsigned prev;
    double holdStart, lastTime, repeatAt, leaveAt;
    int port;
    bool armed;  // Back + Start must be let go once before they open it
    Guide() : r(0), frameEl(0), menuEl(0), skinEl(0), frame(0), menu(0), hud(0), state(Off),
              prev(0), holdStart(-1), lastTime(-1), repeatAt(0), leaveAt(0), port(0), armed(false) {}
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

void set_image(xui::Scene* s, const char* id, const char* path) {
    xui::Node* n = s->find(id);
    if (!n) return;
    xui::Value& v = n->props.set("ImagePath"); v.free(); v.type = xui::TString; v.s = xui::strdup2(path);
}

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
    g.menuEl = g.frameEl ? g.res.scene("GuideMain.xui", err, sizeof err) : 0;
    if (!g.frameEl || !g.menuEl) { xlog("Guide: off, scene load failed: %s", err); return false; }
    g.r = new xui::Renderer(&g.be, &g.res);
    g.r->setFont("font.ttf");
    g.frame = new xui::Scene(g.frameEl, g.skinEl);
    g.frame->setFocus(0);
    g.hud = g.frame->find("HUDRootScene");
    if (!g.hud) g.hud = g.frame->root;
    g.menu = new xui::Scene(g.menuEl, g.skinEl);
    // Frame legends (the 360 fills them from the menu's hidden legend buttons).
    set_text(g.frame, "Legend_A", "Select");
    set_text(g.frame, "Legend_B", "Back");
    set_text(g.frame, "Legend_Y", "Xbox Dashboard");
    set_show(g.frame, "Legend_X", false);
    set_text(g.frame, "GamerTag", "Player");
    // The header: what's playing.
    char title[64]; snprintf(title, sizeof title, "%s", xbe_title_name());
    set_text(g.menu, "LabelSignIn", "Now Playing");
    set_text(g.menu, "labelProfiles", title[0] ? title : "Xbox Game");
    set_text(g.menu, "btnSignIn", "");
    set_text(g.menu, "btnNewProfile", "Resume Game");
    set_text(g.menu, "btnRecoverProfile", "Xbox Dashboard");
    set_show(g.menu, "btnOptions", false);
    g.state = Closed;
    xlog("Guide: ready (%s); hold Back + Start, or Guide / F1", g.res.dir);
    return true;
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

void open_guide() {
    g.state = Opening;
    g.hud->play("ClosedToHalf");
    g.menu->setFocus(g.menu->find("btnNewProfile"));
    update_clock();
    show_port();
    xinput_guide_mask(true);
    ds_set_duck(0.3f);
    xlog("Guide: open (pad %d)", g.port + 1);
}

void close_guide() {
    g.state = Closing;
    g.hud->play("HalfToClosed");
    xinput_guide_mask(false);
    ds_set_duck(1.0f);
}

void press(const char* id) {
    if (!strcmp(id, "btnNewProfile") || !strcmp(id, "btnSignIn")) close_guide();
    else if (!strcmp(id, "btnRecoverProfile")) {
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
        if (toggle || (down & GUIDE_B)) { close_guide(); break; }
        if (down & GUIDE_Y) { press("btnRecoverProfile"); break; }
        if (down & GUIDE_A) { if (g.menu->focus) { g.menu->press(); press(g.menu->focus->id()); } break; }
        if (b & (GUIDE_UP | GUIDE_DOWN)) {
            if ((down & (GUIDE_UP | GUIDE_DOWN)) || t >= g.repeatAt) {
                g.menu->navigate((b & GUIDE_UP) ? "up" : "down");
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
    (void)w; (void)h;
    return true;
}

void guide_draw(int w, int h) {
    // The scenes are 1120x770 4:3 canvases; fit the height and stretch x to
    // the window the way the game image is stretched.
    float s = h / 770.0f;
    float kx = (w / 1120.0f) / s;
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
