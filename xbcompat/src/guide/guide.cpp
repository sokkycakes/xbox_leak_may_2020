// The Guide: an overlay over every title, drawn by xbcompat at present time.
// The frame (hudbkgnd) and the menus (MainMenuSignedIn, Options) are the
// Games for Windows - LIVE Guide's own XUI scenes from xlive.dll, converted
// to .xui by tools/guide/mkguide.py and played on the frolickdash XUI
// runtime; this file is their host: a GL backend, a directory of assets, the
// open/close logic and the menu actions (xlive fills its texts from code,
// so they are set here).
//
// Its home is two Blades tabs (tools/guide/mkhome.py): Game, the game being
// played in a box like the gamer card (its title image, name and time this
// session; A opens a fuller view) over Quick Launch, Downloads and Exit
// Game, and Settings (Volume, Vibration).
//
// Hold Back + Start for half a second (or press the Guide button, or F1) to
// open it.  While it is open the title keeps running, sees idle pads and is
// turned down.  LT / RT (or Left / Right) switch tabs, A picks, B goes back
// or closes, Y goes to the dashboard.
#include "xui.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

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
// The title's XBE and its image reader (src/xbeimage.c); weak, so a host
// without them (kazeta-guide) shows the game box without the picture.
__attribute__((weak)) const char *xbe_file_path(void);
__attribute__((weak)) bool xbe_image_read(const char *xbe, const char *section, unsigned char **bgra, int *w, int *h);
__attribute__((weak)) bool xpr_image_read(const char *path, unsigned char **bgra, int *w, int *h);
// kazeta-guide: the cart's icon as BGRA rows (malloc'd), when it has one.
__attribute__((weak)) bool kazeta_title_image(unsigned char **bgra, int *w, int *h);
// Quick Launch's start of another title and the game card's folder
// (misc.c, io.c); a host without them lists the games but can't start one.
__attribute__((weak)) void guide_launch(const char *nt_dir, const char *xbe);
__attribute__((weak)) bool fs_card_dir(char *out, size_t outlen);
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

// titleimage.png is the title's own picture, decoded from its XBE; the
// list pages' pictures (games' title images, saves' images) are made the
// same way and kept by name.
struct MemImage { char name[96]; unsigned char* xtx; int len; };
struct DirResources : xui::Resources {
    char dir[512];
    unsigned char* tile; int tileLen;  // titleimage.png as .xtx bytes
    MemImage mem[96]; int nmem;
    DirResources() : tile(0), tileLen(0), nmem(0) {}
    // name: "<name>.xtx"; bgra: w*h pixels
    bool add(const char* name, const unsigned char* bgra, int w, int h) {
        for (int i = 0; i < nmem; i++) if (!strcasecmp(mem[i].name, name)) return true;
        if (nmem == (int)(sizeof mem / sizeof mem[0])) return false;
        MemImage& m = mem[nmem++];
        snprintf(m.name, sizeof m.name, "%s", name);
        m.len = 12 + w * h * 4;
        m.xtx = new unsigned char[m.len];
        memcpy(m.xtx, "XTX1", 4); memcpy(m.xtx + 4, &w, 4); memcpy(m.xtx + 8, &h, 4);
        memcpy(m.xtx + 12, bgra, w * h * 4);
        return true;
    }
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
        if (!strcasecmp(nm, "titleimage.xtx")) {
            if (!tile) return false;
            *data = new unsigned char[tileLen];
            memcpy(*data, tile, tileLen); *len = tileLen;
            return true;
        }
        for (int i = 0; i < nmem; i++)
            if (!strcasecmp(mem[i].name, nm)) {
                *data = new unsigned char[mem[i].len];
                memcpy(*data, mem[i].xtx, mem[i].len); *len = mem[i].len;
                return true;
            }
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
typedef void (APIENTRY *PFNACTIVETEX)(GLenum);
typedef void (APIENTRY *PFNBINDBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNBLENDEQ)(GLenum);

struct GLBackend : xui::Backend {
    int W, H; float kx;  // window size; extra x stretch (non-square output)
    PFNBLENDSEP blendSep; PFNBLENDEQ blendEq; PFNACTIVETEX activeTex; PFNBINDBUFFER bindBuffer;
    void procs() {
        if (blendSep) return;
        blendSep = (PFNBLENDSEP)SDL_GL_GetProcAddress("glBlendFuncSeparate");
        blendEq = (PFNBLENDEQ)SDL_GL_GetProcAddress("glBlendEquation");
        activeTex = (PFNACTIVETEX)SDL_GL_GetProcAddress("glActiveTexture");
        bindBuffer = (PFNBINDBUFFER)SDL_GL_GetProcAddress("glBindBuffer");
    }
    GLBackend() : W(0), H(0), kx(1), blendSep(0), blendEq(0), activeTex(0), bindBuffer(0) {}
    // Loading runs outside the overlay's state save (the scenes load before
    // the first draw), so the title's bindings are put back by hand.
    xui::Texture upload(const xui::Image& img) {
        procs();
        GLint prevTex = 0, prevPbo = 0, align = 4, row = 0, skipRows = 0, skipPix = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevPbo);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPix);
        if (prevPbo && bindBuffer) bindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, img.pix);
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPix);
        if (prevPbo && bindBuffer) bindBuffer(GL_PIXEL_UNPACK_BUFFER, prevPbo);
        glBindTexture(GL_TEXTURE_2D, prevTex);
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
        // Immediate mode: the title's client arrays and buffers stay as they were.
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < ni; i++) {
            if (idx[i] >= nv) continue;
            const xui::Vertex& p = v[idx[i]];
            glColor4f(p.r, p.g, p.b, p.a);
            glTexCoord2f(p.u, p.v);
            glVertex2f(p.x, p.y);
        }
        glEnd();
    }
    void begin(int w, int h, float stretch) {
        W = w; H = h; kx = stretch;
        procs();
        glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_ALPHA_TEST);
        glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glDisable(GL_FOG);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_COLOR_LOGIC_OP); glDisable(GL_POLYGON_STIPPLE);
        for (int i = 0; i < 6; i++) glDisable(GL_CLIP_PLANE0 + i);
        // The title's other texture units would modulate every Guide pixel.
        for (int u = 7; u >= 0; u--) {
            if (activeTex) activeTex(GL_TEXTURE0 + u); else if (u) continue;
            glDisable(GL_TEXTURE_1D); glDisable(GL_TEXTURE_2D); glDisable(GL_TEXTURE_3D);
            glDisable(GL_TEXTURE_CUBE_MAP); glDisable(GL_TEXTURE_RECTANGLE_ARB);
            glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T);
            glDisable(GL_TEXTURE_GEN_R); glDisable(GL_TEXTURE_GEN_Q);
        }
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
enum { SndOpen, SndClose, SndFocus, SndSelect, SndBack, SndInactive, SndTab, SndCount };
const char* const soundFiles[SndCount] = { "HUD_open.wav", "HUD_close.wav", "btn_Focus.wav", "btn_Select.wav", "btn_Back.wav",
                                           "btn_InactiveSelect.wav", "tab_Switch.wav" };

struct Guide {
    DirResources res;
    GLBackend be;
    xui::Renderer* r;
    xui::Element *frameEl, *mainEl, *settingsEl, *skinEl, *cardEl, *volumeEl, *vibrationEl;
    xui::Element *tabsEl, *tabsSkinEl, *gameEl, *gameSkinEl;
    xui::Element *recentEl, *downloadsEl, *savesEl, *confirmEl;
    // menu: the page showing.  main and settings are the home's Game and
    // Settings tabs; volume and vibration are Settings' own pages, game the
    // Game tab's fuller view of the game.
    xui::Scene *frame, *main, *settings, *volume, *vibration, *game, *menu;
    // Frolick's list pages: Quick Launch (Recent Games), Downloads (Active
    // Downloads), Game Storage (the title's Saved Games) and its Yes / No.
    xui::Scene *recent, *downloads, *saves, *confirm;
    xui::Scene* card;  // the game box, drawn in the main menu's ctlGamerCard
    xui::Scene* tabs;  // the Blades tab strip over the home's pages
    xui::Node* strip;  // ... its pieces and 1To2 / 2To1 timelines
    int tab;           // 0 Game, 1 Settings
    double started;    // when the title started (the Guide loads on its first frame)
    char startedAt[16];
    int shownMinutes;
    bool full;         // the frame expanded (HalfToFull) for the game view
    const char* launchDir;  // Quick Launch: the game to start once the Guide is away
    double listedAt;   // when the Downloads page last read the queue
    xui::Node* hud;  // HUDRootScene: owns the open/close timelines
    State state;
    unsigned prev;
    double holdStart, lastTime, repeatAt, leaveAt;
    int port;
    bool armed;  // Back + Start must be let go once before they open it
    int volumeLevel;  // 0..10
    bool vibrationOn;
    Sound sounds[SndCount];
    Guide() : r(0), frameEl(0), mainEl(0), settingsEl(0), skinEl(0), cardEl(0), volumeEl(0), vibrationEl(0),
              tabsEl(0), tabsSkinEl(0), gameEl(0), gameSkinEl(0), recentEl(0), downloadsEl(0), savesEl(0), confirmEl(0),
              frame(0), main(0), settings(0), volume(0), vibration(0), game(0), menu(0),
              recent(0), downloads(0), saves(0), confirm(0), card(0),
              tabs(0), strip(0), tab(0), started(0), shownMinutes(-1), full(false), launchDir(0), listedAt(0),
              hud(0), state(Off), prev(0), holdStart(-1), lastTime(-1), repeatAt(0), leaveAt(0), port(0),
              armed(false), volumeLevel(10), vibrationOn(true) {}
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
// A slider's label and value sit tight around its track, and the gamer
// card's lines are packed 25 units apart: grown as far as the rest of the
// text, they'd run into each other.
float text_zoom_for(xui::Node* n, float zoom) {
    xui::Node* o = n->owner;
    // The tab strip is drawn at 0.72 (its 855 units to the panel's 616): its
    // titles, 20 pt, then come out at the pages' heading size (18 pt).
    if (o && !strncmp(o->id(), "tab_", 4)) return zoom * 1.25f;
    if (o && o->cls && !strcmp(o->cls->name, "XuiSlider")) {
        // its number has a box just three digits wide
        if (!strcmp(n->id(), "Text_Slider")) return 1;
        if (zoom > 1.15f) return 1.15f;
    }
    return zoom;
}

// Frolick's page has white text for its slate band; on the Guide's grey
// panel it takes the Guide's black (sokky), keeping each text's fade, and
// so does the clock the full view would turn white (the prompts stay white).
bool is_page(xui::Scene* m) {
    return m && (m == g.game || m == g.recent || m == g.downloads || m == g.saves || m == g.confirm);
}

xui::Color text_color_for(xui::Node* n, xui::Color col) {
    if (!is_page(g.menu)) return col;
    if (!strcmp(n->id(), "DateTimeTextId")) return col & 0xff000000;
    for (xui::Node* p = n; p; p = p->parent)
        if (p == g.menu->root) return col & 0xff000000;
    return col;
}

// Its dark shadows, made for white text, become the light one its rows
// have (0x5aebebeb: the header's 0xbe was much stronger, sokky).
xui::Color text_shadow_for(xui::Node* n, xui::Color col) {
    if (!is_page(g.menu) || (col & 0xff) >= 0x80) return col;
    for (xui::Node* p = n; p; p = p->parent)
        if (p == g.menu->root) return 0x5aebebeb;
    return col;
}

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

// The frame's legend buttons are never focused; GFWL shows them at their
// visual's first frame (the lit look), not at Normal.
void rest(xui::Node* v) {
    if (v->timeline) { v->time = 0; v->playing = false; v->apply(); }
    for (int i = 0; i < v->children.n; i++) rest(v->children[i]);
}

void rest_visual(xui::Scene* s, const char* id) {
    xui::Node* n = s->find(id);
    if (!n || !n->visual) return;
    rest(n->visual);
    // B and Y's lit frame adds coloured and dark glow rings (offset below
    // the disc, so their circles look bigger and lower than A's) and a
    // brighter shine; draw all three buttons alike, as A's plain visual
    for (int i = 0; i < n->visual->children.n; i++) {
        xui::Node* c = n->visual->children[i];
        const char* cid = xui::propS(c->props, "Id");
        size_t l = strlen(cid);
        float op = -1;
        if (l >= 3 && strcmp(cid + l - 3, "glo") == 0) op = 0;
        else if (strcmp(cid, "shine") == 0) op = 0.4f;
        if (op < 0) continue;
        xui::Value& v = c->props.set("Opacity"); v.free(); v.type = xui::TFloat; v.f = op;
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
    float v = g.volumeLevel / 10.0f;
    ds_set_volume(v * v);
    xinput_guide_vibration(g.vibrationOn);
}

void read_settings() {
    char path[600]; settings_path(path, sizeof path);
    FILE* f = fopen(path, "r");
    if (f) {
        int v = 10, r = 1;
        if (fscanf(f, "volume=%d vibration=%d", &v, &r) >= 1) {
            g.volumeLevel = v < 0 ? 0 : v > 10 ? 10 : v;
            g.vibrationOn = r != 0;
        }
        fclose(f);
    }
    apply_settings();
}

void write_settings() {
    char path[600]; settings_path(path, sizeof path);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "volume=%d vibration=%d\n", g.volumeLevel, g.vibrationOn ? 1 : 0);
    fclose(f);
}

void settings_text();
void load_sound(Sound& snd, const char* name);

// ---- the home ------------------------------------------------------------
// In the menus' units (the scenes' 1120x770, the panel's face x 46-662):
// the tab strip's band (GuideTabs.xui's tabClip, 855 wide at 144,84) is
// drawn scaled to the face's width with its top at TAB_Y, its bars then
// filling about TAB_Y+30 to TAB_Y+91; the Game tab's box and the Settings
// tab's first row start at CARD_Y.
const float FACE_X = 46, FACE_W = 616, TAB_Y = 46, CARD_Y = 150;
const float STRIP_X = 144, STRIP_Y = 84, STRIP_W = 855;
// The slant between the two tabs (tab_active_C, 533-632) and how far each
// title's box keeps from it.
const float SLANT_X = 583, SLANT_GAP = 40;

const char* title_name() {
    const char* t = xbe_title_name();
    return t && t[0] ? t : "Xbox Game";
}

// The Game tab's rows keep GFWL's row visual (icon, then text) without
// the icon: the text starts where the Settings rows' does.
void no_icon(xui::Scene* s, const char* id) {
    xui::Node* n = s->find(id);
    if (!n || !n->visual) return;
    for (int i = 0; i < n->visual->children.n; i++) {
        xui::Node* c = n->visual->children[i];
        const char* cid = c->id();
        if (!strcmp(cid, "XuiImagePresenter1")) {
            xui::Value& v = c->props.set("Opacity"); v.free(); v.type = xui::TFloat; v.f = 0;
        } else if (!strcmp(cid, "text_Label") || !strcmp(cid, "text_Label1")) {
            xui::Vec3 z = {0, 0, 0};
            xui::Vec3 p = xui::propV(c->props, "Position", z); p.x = -17.07f;
            xui::Value& v = c->props.set("Position"); v.free(); v.type = xui::TVector; v.v = p;
            xui::Value& w = c->props.set("Width"); w.free(); w.type = xui::TFloat; w.f = 455;
        }
    }
}

// The game box without the card's header strip (sokky: "less tall"): the
// picture, name and session move up into it and the box and its highlight
// shrink by as much, 141 to CARD_H.
const float CARD_CUT = 46, CARD_H = 80;
void compact_card() {
    set_show(g.card, "gamerTagText", false);
    hide_drawn(g.card, "graphic_memsilver");
    hide_drawn(g.card, "DivideLine");
    xui::Node* off = g.card->find("offlineBaseScene");
    for (int i = 0; off && i < off->children.n; i++) {
        xui::Node* c = off->children[i];
        xui::Vec3 z = {0, 0, 0};
        xui::Vec3 p = xui::propV(c->props, "Position", z);
        if (p.y < 30) continue;  // the strip itself
        p.y -= CARD_CUT;
        // the two lines centred on the picture (they sat by the strip)
        if (c->cls && !strcmp(c->cls->name, "XuiLabel")) p.y += 9;
        xui::Value& v = c->props.set("Position"); v.free(); v.type = xui::TVector; v.v = p;
    }
    static const char* boxes[] = { "baseMetaPaneBack", "baseMetaPane", "offlineBaseScene" };
    for (unsigned i = 0; i < sizeof boxes / sizeof boxes[0]; i++)
        if (xui::Node* b = g.card->find(boxes[i])) {
            xui::Value& v = b->props.set("Height"); v.free(); v.type = xui::TFloat; v.f = CARD_H;
        }
}

// The title image ($$XTIMAGE) as an .xtx for titleimage.png.
void load_title_image() {
    unsigned char* px = 0; int w = 0, h = 0;
    const char* xbe = xbe_file_path ? xbe_file_path() : 0;
    if (kazeta_title_image && kazeta_title_image(&px, &w, &h)) {
        // kazeta-guide: the cart's own icon
    } else if (!xbe || !xbe_image_read || !xbe_image_read(xbe, "$$XTIMAGE", &px, &w, &h)) {
        xlog("Guide: no title image");
        return;
    }
    g.res.tileLen = 12 + w * h * 4;
    g.res.tile = new unsigned char[g.res.tileLen];
    memcpy(g.res.tile, "XTX1", 4);
    memcpy(g.res.tile + 4, &w, 4); memcpy(g.res.tile + 8, &h, 4);
    memcpy(g.res.tile + 12, px, w * h * 4);
    free(px);
    xlog("Guide: title image %dx%d", w, h);
}

// Saved games: the title's folders in <hdd>/partition1/UDATA/<title id>.
int saved_games() {
    const char* hdd = fs_hdd_root();
    if (!hdd) return 0;
    char dir[600]; int n = 0;
    for (int i = 0; i < 2 && !n; i++) {
        snprintf(dir, sizeof dir, i ? "%s/partition1/UDATA/%08lX" : "%s/partition1/UDATA/%08lx", hdd, (unsigned long)xbe_title_id());
        DIR* d = opendir(dir);
        if (!d) continue;
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char sub[900]; snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
            DIR* s2 = opendir(sub);
            if (s2) { n++; closedir(s2); }
        }
        closedir(d);
    }
    return n;
}

void game_page() {
    const char* name = title_name();
    set_text(g.game, "labHeader", "Now Playing");
    set_text(g.game, "labGameTitle", name);
    set_image(g.game, "imgGameIcon", g.res.tile ? "titleimage.png" : "");
    set_text(g.game, "btnViewAchievements", "Controller Configuration");  // sokky
    set_show(g.game, "btnBrowseDownloads", false);       // Install Game: the game is running
    set_str(g.game, "btnManageGameFiles", "NavDown", "");
    hide_drawn(g.game, "body");  // Frolick's slate band: the page sits on the Guide's panel (sokky)
    static const char* legends[] = { "legend_A", "legend_B", "legend_X", "legend_Y" };  // the frame has its own
    for (unsigned i = 0; i < sizeof legends / sizeof legends[0]; i++) set_show(g.game, legends[i], false);
    // a host without an XBE (kazeta-guide) runs a Windows game card in Wine
    bool windows = !xbe_file_path;
    set_text(g.game, "labRecentAchievementsText", "Platform");
    set_text(g.game, "labRecentAchievementsNum", windows ? "Windows" : "Xbox");
    set_text(g.game, "labRuntimeText", "Runtime");
    set_text(g.game, "labRuntimeNum", windows ? "Wine" : "xbcompat");
    char buf[16]; snprintf(buf, sizeof buf, "%d", saved_games());
    set_text(g.game, "labAchievementsNum", buf);
    // where the XBE is, both paths resolved (either may be relative)
    const char* xbe = xbe_file_path ? xbe_file_path() : 0;
    char rx[PATH_MAX], rh[PATH_MAX];
    bool onHdd = xbe && fs_hdd_root() && realpath(xbe, rx) && realpath(fs_hdd_root(), rh) &&
                 !strncmp(rx, rh, strlen(rh)) && rx[strlen(rh)] == '/';
    set_text(g.game, "labGamerPointsNum", windows ? "Game card" : onHdd ? "Hard drive" : "Disc");
}

// The time this session, "h:mm", on the game box.
void session_text() {
    int m = (int)((now_s() - g.started) / 60);
    if (m == g.shownMinutes) return;
    g.shownMinutes = m;
    char buf[16]; snprintf(buf, sizeof buf, "%d:%02d", m / 60, m % 60);
    if (g.card) set_text(g.card, "credText", buf);
}


// ---- Frolick's list pages ------------------------------------------------
// Quick Launch, Downloads and Game Storage are Frolick's own pages (sokky,
// 2026-10-25): its Recent Games, Active Downloads and Saved Games, filled
// here from what Frolick keeps on the hard disk, as Frolick's hub fills
// them.  The hard disk's folders are named as the titles made them, so
// paths are matched without case.

// dir/name, its name as the folder spells it
bool ci_path(const char* dir, const char* name, char* out, int n) {
    DIR* d = opendir(dir);
    if (!d) return false;
    bool ok = false;
    while (struct dirent* e = readdir(d))
        if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = true; break; }
    closedir(d);
    return ok;
}

// root + "a/b/c", each part matched without case
bool ci_walk(const char* root, const char* rel, char* out, int n) {
    if (!root) return false;
    char cur[1024]; snprintf(cur, sizeof cur, "%s", root);
    char part[256];
    while (*rel) {
        const char* sl = strchr(rel, '/');
        int l = sl ? (int)(sl - rel) : (int)strlen(rel);
        snprintf(part, sizeof part, "%.*s", l, rel);
        char next[1024];
        if (!ci_path(cur, part, next, sizeof next)) return false;
        snprintf(cur, sizeof cur, "%s", next);
        rel += l + (sl ? 1 : 0);
    }
    snprintf(out, n, "%s", cur);
    return true;
}

char* read_file(const char* path, long* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    if (l < 0 || l > (1 << 20)) { fclose(f); return 0; }
    char* p = new char[l + 2];
    l = (long)fread(p, 1, l, f);
    p[l] = p[l + 1] = 0;
    fclose(f);
    if (len) *len = l;
    return p;
}

// the next line of p (cut in place, its \r gone), or 0 at the end
char* next_line(char** p) {
    if (!*p || !**p) return 0;
    char* line = *p;
    char* nl = strchr(line, '\n');
    if (nl) { *nl = 0; *p = nl + 1; } else *p = line + strlen(line);
    int n = (int)strlen(line);
    if (n && line[n - 1] == '\r') line[n - 1] = 0;
    return line;
}

int split_tabs(char* s, char** f, int n) {
    int k = 0;
    f[k++] = s;
    for (; *s && k < n; s++)
        if (*s == '\t') { *s = 0; f[k++] = s + 1; }
    return k;
}

void format_mb(char* out, int n, int mb) {
    if (mb >= 1024) snprintf(out, n, "%d.%d GB", mb / 1024, (mb % 1024) * 10 / 1024);
    else snprintf(out, n, "%d MB", mb);
}

unsigned rd32le(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

// An XBE's title ID and name (its certificate), the name as ASCII.
bool xbe_info(const char* path, ULONG* tid, char* name, int n) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    unsigned char h[0x1000];
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    if (got < 0x178 || memcmp(h, "XBEH", 4)) return false;
    unsigned cert = rd32le(h + 0x118) - rd32le(h + 0x104);
    if (cert + 0x5c > got) return false;
    *tid = rd32le(h + cert + 8);
    int k = 0;
    for (int i = 0; i < 40 && k < n - 1; i++) {
        unsigned c = h[cert + 0xc + 2 * i] | h[cert + 0xd + 2 * i] << 8;
        if (!c) break;
        name[k++] = c < 0x80 ? (char)c : '?';
    }
    name[k] = 0;
    return true;
}

// An image for a row: a picture made from bgra under `name`, else the
// Blades icon (Frolick's default for its rows).
void row_image(char* out, int n, const char* name, unsigned char* px, int w, int h, const char* fallback) {
    if (px && g.res.add(name, px, w, h)) {
        snprintf(out, n, "%.*s.png", (int)strlen(name) - 4, name);
    } else snprintf(out, n, "%s", fallback);
    free(px);
}

// -- Quick Launch: Frolick's recently played games --------------------------
// Frolick writes each launch's key at the top of its T:\recent.txt (the
// dashboard title's TDATA): an XBE's title ID in hex (a cart's stub XBE
// included).  Listed are the games those keys name that are here now, the
// hard disk's E:\Games folders and the game card, newest first, the game
// being played left out.
struct Game { char name[96], ntdir[600], img[100]; const char* platform; };
Game games[20]; int ngames;

void scan_recent() {
    ngames = 0;
    const char* hdd = fs_hdd_root();
    char path[1024];
    if (!ci_walk(hdd, "partition1/TDATA/FFFE0000/recent.txt", path, sizeof path)) return;
    char* text = read_file(path, 0);
    if (!text) return;
    // the games here: E:\Games\<folder>\default.xbe, and the card's
    struct Here { ULONG tid; char name[96], ntdir[600], xbe[1024]; };
    static Here here[64]; int nhere = 0;
    char gdir[1024];
    if (ci_walk(hdd, "partition1/Games", gdir, sizeof gdir)) {
        if (DIR* d = opendir(gdir)) {
            while (struct dirent* e = readdir(d)) {
                if (e->d_name[0] == '.' || nhere == 64) continue;
                char sub[1100]; snprintf(sub, sizeof sub, "%s/%s", gdir, e->d_name);
                Here& h = here[nhere];
                if (!ci_path(sub, "default.xbe", h.xbe, sizeof h.xbe) || !xbe_info(h.xbe, &h.tid, h.name, sizeof h.name)) continue;
                if (!h.name[0]) snprintf(h.name, sizeof h.name, "%s", e->d_name);
                snprintf(h.ntdir, sizeof h.ntdir, "\\Device\\Harddisk0\\Partition1\\%s\\%s", strrchr(gdir, '/') + 1, e->d_name);
                nhere++;
            }
            closedir(d);
        }
    }
    char card[1024];
    if (fs_card_dir && nhere < 64 && fs_card_dir(card, sizeof card)) {
        Here& h = here[nhere];
        if (ci_path(card, "default.xbe", h.xbe, sizeof h.xbe) && xbe_info(h.xbe, &h.tid, h.name, sizeof h.name)) {
            snprintf(h.ntdir, sizeof h.ntdir, "\\Device\\GameCard0");
            nhere++;
        }
    }
    char* p = text;
    while (char* key = next_line(&p)) {
        if (ngames == 20) break;
        ULONG tid = (ULONG)strtoul(key, 0, 16);
        if (!key[0] || tid == xbe_title_id()) continue;
        for (int i = 0; i < nhere; i++) {
            if (here[i].tid != tid) continue;
            Game& gm = games[ngames++];
            snprintf(gm.name, sizeof gm.name, "%s", here[i].name);
            snprintf(gm.ntdir, sizeof gm.ntdir, "%s", here[i].ntdir);
            // a Kazeta cart's stub XBE (a Windows card's, wincart.c, or a DS
            // cart's in E:\Games, dstitle.py) has its .wincart beside it,
            // naming the cart's runtime
            char dir[1024], wc[1100]; snprintf(dir, sizeof dir, "%s", here[i].xbe);
            if (char* sl = strrchr(dir, '/')) *sl = 0;
            gm.platform = "Xbox";
            if (ci_path(dir, ".wincart", wc, sizeof wc)) {
                gm.platform = "Windows";
                if (char* text = read_file(wc, 0)) {
                    char* p = text;
                    while (char* line = next_line(&p))
                        if (!strncasecmp(line, "runtime=", 8) && !strncasecmp(line + 8, "nds", 3)) gm.platform = "Nintendo DS";
                    delete[] text;
                }
            }
            unsigned char* px = 0; int w = 0, h = 0;
            char nm[64]; snprintf(nm, sizeof nm, "game_%08lx.xtx", (unsigned long)tid);
            if (!xbe_image_read || !xbe_image_read(here[i].xbe, "$$XTIMAGE", &px, &w, &h)) px = 0;
            row_image(gm.img, sizeof gm.img, nm, px, w, h, "frolick_ico_64x_games.png");
            break;
        }
    }
    delete[] text;
}

// -- Downloads: Frolick's download queue -----------------------------------
// Frolick's Downloads node keeps the queue in E:\FROLICK\STORE\queue.txt,
// the active download first ("q id go|pause bytes kind title"), and its
// helper (frolick-store, running beside the game) reports in status.txt;
// the page reads both, and A / X change the queue as Frolick's page does.
// Streaming Downloads still coming in are listed too, as Frolick's page
// lists them (its d00cfd0d, 240cc4f6): status.txt's "streaming id in
// bytes 1|0|2 title" lines; the one filling (1), else a paused one (2), is
// the active item when the queue has none, the others pend after the
// queue.  Start Now, Pause / Resume and Remove on one go to the helper
// through play.txt (f, z / r, x), its p line kept.
struct Download { char id[96], name[128], kind[32]; long long bytes; int sizeMB; float doneMB; int state; };
enum { DlPending, DlDownloading, DlPaused };
Download dls[16]; int ndl;
int dlOnline;              // -1 the helper isn't running
char dlActiveState[16], dlActiveDetail[64];
struct Stream { char id[96], name[128]; long long in, bytes; int fill; };  // fill: 0 pending, 1 filling, 2 paused
Stream streams[16]; int nstreams;
char streamFirst[96];          // Start Now on a stream, until the helper drops it
char streamGone[16][96]; int nstreamGone;  // removed, until no longer listed
struct PauseCmd { char id[96]; bool pause; };
PauseCmd pauses[16]; int npauses;          // z / r, until the helper says

bool store_path(const char* file, char* out, int n) {
    char dir[1024];
    if (!ci_walk(fs_hdd_root(), "partition1/FROLICK/STORE", dir, sizeof dir)) return false;
    if (!ci_path(dir, file, out, n)) snprintf(out, n, "%s/%s", dir, file);
    return true;
}

void save_play();

void load_downloads() {
    ndl = 0; nstreams = 0; dlOnline = -1; dlActiveState[0] = dlActiveDetail[0] = 0;
    bool listed[16] = { false };  // removed streams the helper still lists
    char path[1100];
    char* text = store_path("queue.txt", path, sizeof path) ? read_file(path, 0) : 0;
    if (text && strstr(text, "\nend")) {
        char* p = text;
        while (char* line = next_line(&p)) {
            char* f[6];
            if (strncmp(line, "q\t", 2) || ndl == 16 || split_tabs(line, f, 6) != 6) continue;
            Download& d = dls[ndl];
            memset(&d, 0, sizeof d);
            snprintf(d.id, sizeof d.id, "%s", f[1]);
            snprintf(d.kind, sizeof d.kind, "%s", f[4]);
            snprintf(d.name, sizeof d.name, "%s", f[5]);
            d.bytes = atoll(f[3]);
            d.sizeMB = (int)(d.bytes / (1024 * 1024));
            d.state = ndl == 0 ? (!strcmp(f[2], "pause") ? DlPaused : DlDownloading) : DlPending;
            ndl++;
        }
    }
    delete[] text;
    // status.txt: the helper rewrites it every second
    struct stat st;
    text = store_path("status.txt", path, sizeof path) && !stat(path, &st) && time(0) - st.st_mtime < 10 ? read_file(path, 0) : 0;
    if (text && strstr(text, "\nend")) {
        char* p = text;
        while (char* line = next_line(&p)) {
            char* f[6];
            int k = split_tabs(line, f, 6);
            int i = -1;
            if (k >= 2) for (int j = 0; j < ndl; j++) if (!strcmp(dls[j].id, f[1])) i = j;
            if (!strcmp(f[0], "streaming") && k >= 6 && nstreams < 16) {
                bool gone = false;
                for (int r = 0; r < nstreamGone; r++)
                    if (!strcmp(streamGone[r], f[1])) { gone = true; listed[r] = true; }
                if (!gone) {
                    Stream& sm = streams[nstreams++];
                    snprintf(sm.id, sizeof sm.id, "%s", f[1]);
                    sm.in = atoll(f[2]); sm.bytes = atoll(f[3]); sm.fill = atoi(f[4]);
                    snprintf(sm.name, sizeof sm.name, "%s", f[5]);
                }
            }
            else if (!strcmp(f[0], "online") && k >= 2) dlOnline = atoi(f[1]);
            else if (!strcmp(f[0], "active") && k >= 5 && i >= 0) {
                long long total = atoll(f[4]);
                if (total > 0) { dls[i].bytes = total; dls[i].sizeMB = (int)(total / (1024 * 1024)); }
                dls[i].doneMB = (float)(atoll(f[3]) / 1024) / 1024.0f;
                if (i == 0) {
                    snprintf(dlActiveState, sizeof dlActiveState, "%s", f[2]);
                    if (k >= 6) snprintf(dlActiveDetail, sizeof dlActiveDetail, "%s", f[5]);
                }
            }
            else if (!strcmp(f[0], "have") && k >= 3 && i >= 0) dls[i].doneMB = (float)(atoll(f[2]) / 1024) / 1024.0f;
            else if (!strcmp(f[0], "done") && k >= 2 && i >= 0) {
                // finished: Frolick moves it to its history when it next runs
                memmove(&dls[i], &dls[i + 1], (ndl - i - 1) * sizeof dls[0]);
                ndl--;
                if (i == 0 && ndl && dls[0].state == DlPending) dls[0].state = DlDownloading;
            }
        }
        // what the helper no longer lists is removed, or finished
        int keep = 0;
        for (int r = 0; r < nstreamGone; r++)
            if (listed[r]) snprintf(streamGone[keep++], sizeof streamGone[0], "%s", streamGone[r]);
        bool changed = keep != nstreamGone;
        nstreamGone = keep;
        if (streamFirst[0]) {
            bool there = false;
            for (int k = 0; k < nstreams; k++) there = there || !strcmp(streams[k].id, streamFirst);
            if (!there) { streamFirst[0] = 0; changed = true; }
            // until the helper says, the one asked for fills
            else for (int k = 0; k < nstreams; k++) {
                if (!strcmp(streams[k].id, streamFirst)) streams[k].fill = 1;
                else if (streams[k].fill == 1) streams[k].fill = 0;
            }
        }
        // a pause or resume the helper has carried out (or a game it dropped)
        keep = 0;
        for (int c = 0; c < npauses; c++) {
            bool pending = false;
            for (int k = 0; k < nstreams; k++) {
                if (strcmp(streams[k].id, pauses[c].id)) continue;
                pending = (streams[k].fill == 2) != pauses[c].pause;
                if (pending) streams[k].fill = pauses[c].pause ? 2 : 0;  // shown as asked meanwhile
            }
            if (pending) pauses[keep++] = pauses[c];
        }
        if (keep != npauses) { npauses = keep; changed = true; }
        if (changed) save_play();
    }
    delete[] text;
}

void save_downloads() {
    char path[1100];
    if (!store_path("queue.txt", path, sizeof path)) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "frolick-queue 1\n");
    for (int i = 0; i < ndl; i++)
        fprintf(f, "q\t%s\t%s\t%lld\t%s\t%s\n", dls[i].id, dls[i].state == DlPaused ? "pause" : "go", dls[i].bytes, dls[i].kind, dls[i].name);
    fprintf(f, "end\n");
    fclose(f);
}

const char* dl_status(int i) {
    if (dls[i].state == DlPaused) return "Paused";
    if (dls[i].state == DlPending) return "Pending";
    if (dlOnline < 0) return "Waiting";
    if (!strcmp(dlActiveState, "nospace")) return "Not enough space";
    if (!strcmp(dlActiveState, "error")) return "Retrying";
    if (!strcmp(dlActiveState, "installing")) return "Installing";
    return "Downloading";
}

void dl_detail(int i, char* out, int n) {
    char size[24], done[24];
    format_mb(size, sizeof size, dls[i].sizeMB);
    if (i == 0 && dlActiveDetail[0] && dls[0].state == DlDownloading) snprintf(out, n, "%s", dlActiveDetail);
    else if (dls[i].state == DlPending && dls[i].doneMB <= 0) snprintf(out, n, "%s", size);
    else { format_mb(done, sizeof done, (int)dls[i].doneMB); snprintf(out, n, "%s of %s", done, size); }
}

// A pending item jumps the queue (the active one waits again at the top);
// the active one pauses or resumes.
void dl_start(int i) {
    if (i < 0 || i >= ndl) return;
    if (i == 0) dls[0].state = dls[0].state == DlDownloading ? DlPaused : DlDownloading;
    else {
        Download d = dls[i];
        memmove(&dls[1], &dls[0], i * sizeof dls[0]);
        dls[0] = d;
        dls[0].state = DlDownloading;
        dls[1].state = DlPending;
    }
    save_downloads();
}

void dl_remove(int i) {
    if (i < 0 || i >= ndl) return;
    memmove(&dls[i], &dls[i + 1], (ndl - i - 1) * sizeof dls[0]);
    ndl--;
    if (i == 0 && ndl && dls[0].state == DlPending) dls[0].state = DlDownloading;
    save_downloads();
}

// The Guide's own line for a stream: its f, x, z or r.
bool play_ours(const char* id) {
    if (!strcmp(id, streamFirst)) return true;
    for (int r = 0; r < nstreamGone; r++) if (!strcmp(id, streamGone[r])) return true;
    for (int c = 0; c < npauses; c++) if (!strcmp(id, pauses[c].id)) return true;
    return false;
}

// play.txt for the helper: the stream to fill first (f), the streams
// removed (x), paused (z) and resumed (r).  The lines Frolick left (its p,
// the game to play, and its commands for other streams) are kept.
void save_play() {
    char path[1100];
    if (!store_path("play.txt", path, sizeof path)) return;
    char kept[16][200]; int nkept = 0;
    if (char* text = read_file(path, 0)) {
        char* p = text;
        while (char* line = next_line(&p)) {
            if (nkept == 16 || strlen(line) < 3 || line[1] != '\t' || !strchr("pfxzr", line[0])) continue;
            if (line[0] == 'f' ? streamFirst[0] != 0 : line[0] != 'p' && play_ours(line + 2)) continue;
            snprintf(kept[nkept++], sizeof kept[0], "%s", line);
        }
        delete[] text;
    }
    char tmp[1110]; snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    for (int k = 0; k < nkept; k++) fprintf(f, "%s\n", kept[k]);
    if (streamFirst[0]) fprintf(f, "f\t%s\n", streamFirst);
    for (int r = 0; r < nstreamGone; r++) fprintf(f, "x\t%s\n", streamGone[r]);
    for (int c = 0; c < npauses; c++) fprintf(f, "%c\t%s\n", pauses[c].pause ? 'z' : 'r', pauses[c].id);
    fprintf(f, "end\n");
    fclose(f);
    rename(tmp, path);
}

// The page's items, as Frolick lists them: the active one first (the
// queue's, else the stream filling), then the queue, then the streams.
// An item is a queue index (q >= 0) or a stream index (s >= 0).
struct DlItem { int q, s; };
bool queue_active() { return ndl && dls[0].state != DlPending; }
int stream_active() {
    if (queue_active()) return -1;
    for (int k = 0; k < nstreams; k++) if (streams[k].fill == 1) return k;
    // none fills while one is paused: the paused one is the active item
    for (int k = 0; k < nstreams; k++) if (streams[k].fill == 2) return k;
    return -1;
}
bool dl_has_active() { return queue_active() || stream_active() >= 0; }
int dl_count() { return ndl + nstreams; }
DlItem dl_item(int i) {
    DlItem it = { -1, -1 };
    int a = stream_active();
    if (a >= 0) { if (i == 0) { it.s = a; return it; } i--; }
    if (i >= 0 && i < ndl) { it.q = i; return it; }
    i -= ndl;
    for (int k = 0; k < nstreams; k++) {
        if (k == a) continue;
        if (i-- == 0) { it.s = k; return it; }
    }
    return it;
}

void dl_item_text(int i, char* name, int nn, char* line, int nl, const char** status, float* progress, bool* theme) {
    DlItem it = dl_item(i);
    *theme = false;
    if (it.s >= 0) {
        Stream& sm = streams[it.s];
        char done[24], size[24];
        format_mb(done, sizeof done, (int)(sm.in / (1024 * 1024)));
        format_mb(size, sizeof size, (int)(sm.bytes / (1024 * 1024)));
        snprintf(name, nn, "%s", sm.name);
        snprintf(line, nl, "Streaming Download   %s of %s", done, size);
        *status = sm.fill == 0 ? "Pending" : sm.fill == 2 ? "Paused" : dlOnline < 0 ? "Waiting" : sm.in >= sm.bytes ? "Checking" : "Downloading";
        *progress = sm.bytes > 0 ? (float)((double)sm.in / (double)sm.bytes) : 0;
        return;
    }
    int d = it.q < 0 ? 0 : it.q;
    char detail[96];
    dl_detail(d, detail, sizeof detail);
    snprintf(name, nn, "%s", dls[d].name);
    snprintf(line, nl, "%s   %s", dls[d].kind, detail);
    *status = dl_status(d);
    *progress = dls[d].sizeMB > 0 ? dls[d].doneMB / dls[d].sizeMB : 0;
    *theme = !strcmp(dls[d].kind, "Theme");
}

// The helper is told to pause or resume a stream; the item shows it at once.
void stream_pause(int k, bool pause) {
    int c = 0;
    while (c < npauses && strcmp(pauses[c].id, streams[k].id)) c++;
    if (c == npauses) { if (npauses == 16) return; npauses++; }
    snprintf(pauses[c].id, sizeof pauses[c].id, "%s", streams[k].id);
    pauses[c].pause = pause;
    streams[k].fill = pause ? 2 : 0;
}

// A on an item, as on Frolick's page: the active one pauses / resumes, a
// pending one starts now (a paused stream resumes and fills first).
// False when A does nothing.
bool dl_press(int i) {
    DlItem it = dl_item(i);
    if (it.q >= 0) { dl_start(it.q); return true; }
    if (it.s < 0) return false;
    if (streams[it.s].fill == 1) {
        stream_pause(it.s, true);
        if (!strcmp(streamFirst, streams[it.s].id)) streamFirst[0] = 0;
        save_play();
        return true;
    }
    if (streams[it.s].fill == 2) stream_pause(it.s, false);
    snprintf(streamFirst, sizeof streamFirst, "%s", streams[it.s].id);
    for (int k = 0; k < nstreams; k++) {
        if (k == it.s) streams[k].fill = 1;
        else if (streams[k].fill == 1) streams[k].fill = 0;
    }
    save_play();
    return true;
}

void dl_remove_item(int i) {
    DlItem it = dl_item(i);
    if (it.q >= 0) { dl_remove(it.q); return; }
    if (it.s < 0 || nstreamGone == 16) return;
    snprintf(streamGone[nstreamGone++], sizeof streamGone[0], "%s", streams[it.s].id);
    if (!strcmp(streamFirst, streams[it.s].id)) streamFirst[0] = 0;
    for (int c = 0; c < npauses; c++)
        if (!strcmp(pauses[c].id, streams[it.s].id)) { pauses[c] = pauses[--npauses]; break; }
    save_play();
    memmove(&streams[it.s], &streams[it.s + 1], (nstreams - it.s - 1) * sizeof streams[0]);
    nstreams--;
}

// -- Game Storage: the title's saved games ---------------------------------
// E:\UDATA\<title id>\<save>: its name from SaveMeta.xbx, its picture from
// SaveImage.xbx (else the title's TitleImage.xbx), "date, size" under it,
// as Frolick's Saved Games page shows a title's saves.
struct Save { char name[96], info[96], dir[1100], img[100]; };
Save saves[48]; int nsaves;

// Name= in a SaveMeta.xbx (UTF-16): the [en] section's, else the first.
void save_name(const char* dir, char* out, int n) {
    snprintf(out, n, "Broken Save");
    char path[1200];
    long len = 0;
    char* raw = ci_path(dir, "SaveMeta.xbx", path, sizeof path) ? read_file(path, &len) : 0;
    if (!raw) return;
    char* a = new char[len / 2 + 2]; int k = 0;
    for (long i = (len >= 2 && (unsigned char)raw[0] == 0xff) ? 2 : 0; i + 1 < len; i += 2) {
        unsigned c = (unsigned char)raw[i] | (unsigned char)raw[i + 1] << 8;
        a[k++] = c < 0x80 ? (char)c : '?';
    }
    a[k] = 0;
    bool found = false, inEn = false;
    char* p = a;
    while (char* line = next_line(&p)) {
        if (line[0] == '[') { inEn = !strncasecmp(line, "[en", 3); continue; }
        if (strncasecmp(line, "Name=", 5)) continue;
        if (!found || inEn) { snprintf(out, n, "%s", line + 5); found = true; }
        if (inEn) break;
    }
    delete[] a; delete[] raw;
}

// bytes in 16 KB blocks, as the dashboard counts a save
long long dir_blocks(const char* dir) {
    long long b = 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    while (struct dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1300]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) b += dir_blocks(p);
        else b += (st.st_size + 16383) / 16384 * 16384;
    }
    closedir(d);
    return b;
}

void scan_saves() {
    nsaves = 0;
    char hex[16], tdir[1024];
    snprintf(hex, sizeof hex, "%08lX", (unsigned long)xbe_title_id());
    char rel[64]; snprintf(rel, sizeof rel, "partition1/UDATA/%s", hex);
    if (!ci_walk(fs_hdd_root(), rel, tdir, sizeof tdir)) return;
    char tpath[1100];
    bool timg = ci_path(tdir, "TitleImage.xbx", tpath, sizeof tpath);
    DIR* d = opendir(tdir);
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || nsaves == 48) continue;
        Save& sv = saves[nsaves];
        snprintf(sv.dir, sizeof sv.dir, "%s/%s", tdir, e->d_name);
        struct stat st;
        if (stat(sv.dir, &st) || !S_ISDIR(st.st_mode)) continue;
        save_name(sv.dir, sv.name, sizeof sv.name);
        char when[48];
        struct tm lt; time_t mt = st.st_mtime; localtime_r(&mt, &lt);
        int hr = lt.tm_hour % 12; if (!hr) hr = 12;
        snprintf(when, sizeof when, "%d/%d/%d %d:%02d %s", lt.tm_mon + 1, lt.tm_mday, lt.tm_year + 1900, hr, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
        long long kb = dir_blocks(sv.dir) / 1024;
        if (kb >= 1024) snprintf(sv.info, sizeof sv.info, "%s, %lld.%lld MB", when, kb / 1024, kb % 1024 * 10 / 1024);
        else snprintf(sv.info, sizeof sv.info, "%s, %lld KB", when, kb);
        unsigned char* px = 0; int w = 0, h = 0;
        char ipath[1300], nm[64];
        unsigned hash = 5381; for (const char* c = sv.dir; *c; c++) hash = hash * 33 + (unsigned char)*c;
        snprintf(nm, sizeof nm, "save_%08x.xtx", hash);
        if (!xpr_image_read || !((ci_path(sv.dir, "SaveImage.xbx", ipath, sizeof ipath) && xpr_image_read(ipath, &px, &w, &h)) ||
                                 (timg && xpr_image_read(tpath, &px, &w, &h)))) px = 0;
        row_image(sv.img, sizeof sv.img, nm, px, w, h, "frolick_ico_64x_games.png");
        nsaves++;
    }
    closedir(d);
}

bool remove_tree(const char* path) {
    struct stat st;
    if (lstat(path, &st)) return false;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        if (!d) return false;
        while (struct dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[1300]; snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            remove_tree(p);
        }
        closedir(d);
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

// -- the pages -------------------------------------------------------------
// Each is a Blades list of six rows (Downloads five, the active one on top
// under "Active", the queue under "Pending"); sel is the item with the
// focus, scroll the first one showing.
struct List { int rows, count, scroll, sel; };
List lRecent = { 6, 0, 0, 0 }, lDownloads = { 5, 0, 0, 0 }, lSaves = { 6, 0, 0, 0 };
// the confirm page's question and what Yes does
enum { AskNone, AskDeleteSave, AskRemoveDownload };
int ask; xui::Scene* askFrom;
char askId[96];  // the download asked about

List* list_of(xui::Scene* m) {
    return m == g.recent ? &lRecent : m == g.downloads ? &lDownloads : m == g.saves ? &lSaves : 0;
}

// Downloads: row 0 is the active download, hidden when there is none
int dl_off() { return dl_has_active() ? 0 : 1; }

void row_id(char* out, int n, int r) { snprintf(out, n, "list_items_%d", r); }

void fill_row(xui::Scene* m, int r, int i) {
    char id[32], sub[48]; row_id(id, sizeof id, r);
    snprintf(sub, sizeof sub, "%s_icon", id);
    if (m == g.downloads) {
        int d = r - dl_off();
        bool on = d >= 0 && d < dl_count();
        set_show(m, id, on);
        if (!on) return;
        char name[128], line[160]; const char* status; float f; bool theme;
        dl_item_text(d, name, sizeof name, line, sizeof line, &status, &f, &theme);
        set_text(m, id, name);
        set_str(m, id, "Data1", line);
        set_str(m, id, "Data3", status);
        set_image(m, sub, theme ? "frolick_ico_64x_Themes.png" : "frolick_ico_64x_GameDownloads.png");
        char bar[48]; snprintf(bar, sizeof bar, "%s_bar", id);
        set_show(m, bar, d == 0 && dl_has_active());
        if (xui::Node* b = m->find(bar)) {
            xui::Value& v = b->props.set("Value"); v.free(); v.type = xui::TFloat; v.f = f < 0 ? 0 : f > 1 ? 1 : f;
            b->setBarValue();
        }
        return;
    }
    List* l = list_of(m);
    set_show(m, id, i < l->count);
    if (i >= l->count) return;
    if (m == g.recent) {
        set_text(m, id, games[i].name);
        set_str(m, id, "Data1", games[i].platform);
        set_image(m, sub, games[i].img);
    } else {
        set_text(m, id, saves[i].name);
        set_str(m, id, "Data1", saves[i].info);
        set_image(m, sub, saves[i].img);
    }
}

void set_legend(const char* id, const char* text);

// the prompts for the focused item
void list_legends() {
    List* l = list_of(g.menu);
    if (!l) l = &lRecent;  // the confirm page
    const char* a = 0, *x = 0, *y = 0;
    if (g.menu == g.recent && l->count) a = guide_launch ? "Play" : 0;
    else if (g.menu == g.saves && l->count) y = "Delete";  // Frolick's Y
    else if (g.menu == g.downloads && dl_count() && l->sel - dl_off() >= 0) {
        DlItem it = dl_item(l->sel - dl_off());
        if (it.q == 0 && queue_active()) a = dls[0].state == DlPaused ? "Resume" : "Pause";
        else if (it.s >= 0 && streams[it.s].fill == 1) a = "Pause";
        else if (it.s >= 0 && streams[it.s].fill == 2) a = "Resume";
        else if (it.q >= 0 || it.s >= 0) a = "Start Now";
        x = "Remove";
    }
    else if (g.menu == g.confirm) a = "Select";
    set_legend("Legend_A", a);
    set_legend("Legend_X", x);
    set_legend("Legend_Y", y);
}

void fill_list(xui::Scene* m) {
    List* l = list_of(m);
    if (!l) return;
    if (m == g.downloads) {
        l->count = dl_count() + dl_off(); if (l->count > l->rows) l->count = l->rows;
        if (l->sel < dl_off()) l->sel = dl_off();
        if (l->sel >= l->count) l->sel = l->count - 1;
        l->scroll = 0;
    } else {
        if (l->sel >= l->count) l->sel = l->count - 1;
        if (l->sel < 0) l->sel = 0;
        if (l->sel < l->scroll) l->scroll = l->sel;
        if (l->sel >= l->scroll + l->rows) l->scroll = l->sel - l->rows + 1;
    }
    for (int r = 0; r < l->rows; r++) fill_row(m, r, l->scroll + r);
    bool empty = m == g.downloads ? dl_count() == 0 : l->count == 0;
    set_show(m, "txt_EmptyList", empty);
    char total[32] = "";
    if (m == g.downloads) {
        if (dl_count() && l->sel - dl_off() >= 0) snprintf(total, sizeof total, "%d of %d", l->sel - dl_off() + 1, dl_count());
        else if (dl_count()) snprintf(total, sizeof total, "%d waiting", dl_count());
    } else if (l->count) snprintf(total, sizeof total, "%d of %d", l->sel + 1, l->count);
    set_text(m, "labTotal", total);
    char id[32]; row_id(id, sizeof id, l->sel - l->scroll);
    m->setFocus(empty ? 0 : m->find(id));
    if (m == g.menu) list_legends();
}

// up / down on a list page; true when the focus moved
bool list_move(int dir) {
    List* l = list_of(g.menu);
    int lo = g.menu == g.downloads ? dl_off() : 0;
    int s = l->sel + dir;
    if (s < lo || s >= l->count) return false;
    l->sel = s;
    fill_list(g.menu);
    return true;
}

// Frolick's page as a Guide page: its slate band and its own prompts go
// (the frame has its own), the header names the Guide's row.
void page_dress(xui::Scene* m, const char* header) {
    hide_drawn(m, "body");
    static const char* legends[] = { "legend_A", "legend_B", "legend_X", "legend_Y", "legend_a", "legend_b", "legend_x", "legend_y" };
    for (unsigned i = 0; i < sizeof legends / sizeof legends[0]; i++) set_show(m, legends[i], false);
    if (header) { set_text(m, "txt_Header", header); set_text(m, "labHeader", header); }
}

void show_menu(xui::Scene* m, const char* focus);

// Shows a home tab: the strip plays its switch, the tab's page comes up.
void show_tab(int t, bool animate) {
    if (g.strip) {
        if (animate) g.strip->play(t ? "1To2" : "2To1");
        else {
            g.strip->time = g.strip->frameTime(t ? "1To2End" : "NormalToEndNormal", 0);
            g.strip->playing = false;
            g.strip->apply();
        }
    }
    g.tab = t;
    show_menu(t ? g.settings : g.main, t ? "btnOnlineStatus" : "ctlGamerCard");
}

// the menus' panel-wide rows, squeezed with the frame on a TV (below)
static xui::Node* rows[64]; static int nrows;
static void collect_rows(xui::Node* n, bool underRow);

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
    g.volumeEl = g.settingsEl ? g.res.scene("GuideVolume.xui", err, sizeof err) : 0;
    g.vibrationEl = g.volumeEl ? g.res.scene("GuideVibration.xui", err, sizeof err) : 0;
    g.tabsSkinEl = g.vibrationEl ? g.res.scene("GuideTabsSkin.xui", err, sizeof err) : 0;
    g.tabsEl = g.tabsSkinEl ? g.res.scene("GuideTabs.xui", err, sizeof err) : 0;
    g.gameEl = g.tabsEl ? g.res.scene("GuideGame.xui", err, sizeof err) : 0;
    g.gameSkinEl = g.gameEl ? g.res.scene("GuideGameSkin.xui", err, sizeof err) : 0;
    if (!g.gameSkinEl) { xlog("Guide: off, scene load failed: %s", err); return false; }
    g.cardEl = g.res.scene("GuideCard.xui", err, sizeof err);  // optional
    // Frolick's list pages (optional: the rows they're behind stay inactive)
    g.recentEl = g.res.scene("GuideRecent.xui", err, sizeof err);
    g.downloadsEl = g.res.scene("GuideDownloads.xui", err, sizeof err);
    g.savesEl = g.res.scene("GuideSaves.xui", err, sizeof err);
    g.confirmEl = g.res.scene("GuideConfirm.xui", err, sizeof err);
    g.started = now_s();
    {
        time_t t = time(0); struct tm lt; localtime_r(&t, &lt);
        int h = lt.tm_hour % 12; if (!h) h = 12;
        snprintf(g.startedAt, sizeof g.startedAt, "%d:%02d%s", h, lt.tm_min, lt.tm_hour < 12 ? "am" : "pm");
    }
    load_title_image();
    g.r = new xui::Renderer(&g.be, &g.res);
    g.r->setFont("font.ttf");
    g.r->defaultTextColor = 0xff000000;
    // The scenes are laid out for Convection, whose ascent is taller than
    // the stand-in font's: with the stand-in's own ascent (0.93 em) every
    // label sat about 3 px high in its row.  1.12 em centres the capitals in
    // the menu rows and the card's header strip.
    g.r->ascentEm = 1.12f;
    g.r->textZoomFor = text_zoom_for;
    g.r->textColorFor = text_color_for;
    g.r->textShadowFor = text_shadow_for;
    g.frame = new xui::Scene(g.frameEl, g.skinEl);
    g.frame->setFocus(0);
    g.hud = g.frame->find("HUDRootScene");
    if (!g.hud) g.hud = g.frame->root;
    g.main = g.menu = new xui::Scene(g.mainEl, g.skinEl);
    g.settings = new xui::Scene(g.settingsEl, g.skinEl);
    g.volume = new xui::Scene(g.volumeEl, g.skinEl);
    g.vibration = new xui::Scene(g.vibrationEl, g.skinEl);
    g.game = new xui::Scene(g.gameEl, g.gameSkinEl);
    g.tabs = new xui::Scene(g.tabsEl, g.tabsSkinEl);
    g.tabs->setFocus(0);
    if (g.recentEl) page_dress(g.recent = new xui::Scene(g.recentEl, g.gameSkinEl), "Quick Launch");
    if (g.downloadsEl) page_dress(g.downloads = new xui::Scene(g.downloadsEl, g.gameSkinEl), "Downloads");
    if (g.savesEl) page_dress(g.saves = new xui::Scene(g.savesEl, g.gameSkinEl), "Game Storage");
    if (g.confirmEl) page_dress(g.confirm = new xui::Scene(g.confirmEl, g.gameSkinEl), 0);
    g.strip = g.tabs->find("tabStrip");
    nrows = 0;
    collect_rows(g.main->root, false); collect_rows(g.settings->root, false);
    collect_rows(g.volume->root, false); collect_rows(g.vibration->root, false);
    // Frame legends (the 360 fills them from the menu's hidden legend buttons).
    set_text(g.frame, "Legend_A", "Select");
    set_text(g.frame, "Legend_B", "Back");
    set_text(g.frame, "Legend_Y", "Xbox Dashboard");
    rest_visual(g.frame, "Legend_A");
    rest_visual(g.frame, "Legend_B");
    rest_visual(g.frame, "Legend_Y");
    hide_drawn(g.frame, "Legend_X");
    hide_drawn(g.frame, "CloseX");          // the PC Guide's mouse close box
    hide_drawn(g.frame, "EvaluationTextId");
    set_text(g.frame, "GamerTag", "Player");
    // The Game tab: GFWL's signed-in main menu with the gamer card (its
    // default focus) as the game's box, then three rows without icons, set
    // in from the tab strip.  The sign-in line and the community bar go.
    set_show(g.main, "scnCommunity", false);
    set_show(g.main, "txtSignInLive", false);
    set_show(g.main, "btnBlank1", true);
    if (xui::Node* b = g.main->find("btnBlank1")) {  // GFWL's spare row, off
        xui::Value& v = b->props.set("Enabled"); v.free(); v.type = xui::TBool; v.b = true;
        b->playState("Normal");
    }
    set_show(g.main, "xboxlive_logo_wide", false);
    static const struct { const char* id; const char* text; float y; } rows[] = {
        { "btnBlank1", "Quick Launch", CARD_Y + 77 }, { "btnQuickChat", "Downloads", CARD_Y + 122 },
        { "btnOptions", "Exit Game", CARD_Y + 167 },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        set_text(g.main, rows[i].id, rows[i].text);
        set_image(g.main, rows[i].id, "");
        no_icon(g.main, rows[i].id);
        set_pos(g.main, rows[i].id, 146, rows[i].y);
        set_str(g.main, rows[i].id, "NavUp", i ? rows[i - 1].id : "ctlGamerCard");
        set_str(g.main, rows[i].id, "NavDown", i + 1 < sizeof rows / sizeof rows[0] ? rows[i + 1].id : "");
    }
    set_pos(g.main, "ctlGamerCard", 146, CARD_Y);
    set_str(g.main, "ctlGamerCard", "NavUp", "");
    set_str(g.main, "ctlGamerCard", "NavDown", "btnBlank1");
    // xlive draws the card into ctlGamerCard from code: GamerCard.xur's
    // offline pane, here with the game: its picture in the tile, its name
    // on the first row and the time this session on the second.
    if (g.cardEl) {
        g.card = new xui::Scene(g.cardEl, g.skinEl);
        g.card->setFocus(0);
        set_show(g.card, "extendedScene", false);
        set_show(g.card, "onlineBaseScene", false);
        set_show(g.card, "offlineBaseScene", true);
        set_text(g.card, "gamerTagText", "Now Playing");
        set_image(g.card, "gamerTileImage", g.res.tile ? "titleimage.png" : "");
        set_text(g.card, "titlesPlayedLabel", title_name());
        set_text(g.card, "titlesPlayedText", "");
        set_text(g.card, "GSText", "This Session");
        hide_drawn(g.card, "GSIcon");
        set_pos(g.card, "GSText", 82.8f, 67.33f);  // under the name, where the icon was
        compact_card();
        set_show(g.card, "achievementsLabel", false);
        set_show(g.card, "achievementsText", false);
    }
    // The fuller view: Frolick's Game Summary page, filled as Frolick's hub
    // fills it for a game on its belt (Platform, Runtime, Saved games,
    // Location), Play Game resuming it here.
    game_page();
    session_text();
    // Settings: GFWL's Personal Settings page with two rows, each opening
    // a GFWL page: Volume (the Voice page's Playback Volume slider) and
    // Vibration (the controller page's Enable Vibration box).
    // On the home's Settings tab its heading gives way to the tab strip and
    // the rows sit where the Game tab's box starts.
    set_show(g.settings, "labelHeading", false);
    set_text(g.settings, "btnOnlineStatus", "Volume");
    set_text(g.settings, "btnVoice", "Vibration");
    set_pos(g.settings, "btnOnlineStatus", 144, CARD_Y);
    set_pos(g.settings, "btnVoice", 144, CARD_Y + 45);
    const char* unused[] = { "btnController", "btnNotifications", "btnNetwork", "btnSQM", "btnAddLive", "btnUpgrade" };
    for (unsigned i = 0; i < sizeof unused / sizeof *unused; i++) set_show(g.settings, unused[i], false);
    set_str(g.settings, "btnVoice", "NavDown", "");
    set_text(g.volume, "labelHeading", "Volume");
    set_text(g.volume, "sliderVolume", "Game Volume");
    const char* voice[] = { "btnPlayback", "btnRecording", "sliderRecording", "sliderVad", "chkMute", "chkAuto",
                            "chkMic", "graphicMetapane", "txtMetaFull", "txtCurrentdevice",
                            "txtCurrentdevicedescription", "txtCurrentdevicebody" };
    for (unsigned i = 0; i < sizeof voice / sizeof *voice; i++) set_show(g.volume, voice[i], false);
    set_str(g.volume, "sliderVolume", "NavDown", "");
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

void set_value(xui::Scene* s, const char* id, const char* key, int v) {
    xui::Node* n = s->find(id);
    if (!n) return;
    xui::Value& x = n->props.set(key); x.free(); x.type = xui::TInt; x.i = v;
}

void settings_text() {
    set_value(g.volume, "sliderVolume", "RangeMin", 0);
    set_value(g.volume, "sliderVolume", "RangeMax", 100);
    set_value(g.volume, "sliderVolume", "Value", g.volumeLevel * 10);
    xui::Node* box = g.vibration->find("chkVibration");
    if (box) {
        bool was = xui::propB(box->props, "Checked", false);
        xui::Value& v = box->props.set("Checked"); v.free(); v.type = xui::TBool; v.b = g.vibrationOn;
        if (was != g.vibrationOn) box->playState(box->focused ? "Focus" : "Normal");
    }
}

// The frame's legend for the page showing: A and B as the page's own
// hidden legend buttons say (GFWL copies them the same way), Y always.
void set_legend(const char* id, const char* text) {
    xui::Node* n = g.frame->find(id);
    if (!n) return;
    xui::Node* t = n->visual ? n->visual : n;
    xui::Value& v = t->props.set("Opacity"); v.free(); v.type = xui::TFloat; v.f = text ? 1 : 0;
    if (text) set_text(g.frame, id, text);
}

const char* page_legend(xui::Scene* m, const char* a, const char* b) {
    xui::Node* n = 0;
    for (int i = 0; i < 2 && !n; i++) n = m->find(i ? b : a);
    return n ? xui::propS(n->props, "Text") : 0;
}

void show_menu(xui::Scene* m, const char* focus) {
    g.menu = m;
    m->setFocus(focus ? m->find(focus) : 0);
    // The Volume page and the game's fuller view have nothing to select.
    set_legend("Legend_A", m == g.volume ? 0 : "Select");
    const char* back = page_legend(m, "btnB", "btnB");
    set_legend("Legend_B", back && back[0] ? back : "Back");
    // Y leaves for the Dashboard from the top screen only, not sub menus (sokky)
    set_legend("Legend_Y", m == g.main || m == g.settings ? "Xbox Dashboard" : 0);
    set_legend("Legend_X", 0);
    if (list_of(m) || m == g.confirm) list_legends();
}

// Opens one of Frolick's pages: the frame opens out to the Blades' width
// (from the home), the page plays its Open.
void open_page(xui::Scene* m, const char* focus) {
    if (!g.full) { g.full = true; g.hud->play("HalfToFull"); }
    show_menu(m, focus);
    if (list_of(m)) fill_list(m);
    if (xui::Node* pg = m->find("page")) pg->play("Open");
}

void ask_confirm(int what, const char* question) {
    ask = what;
    askFrom = g.menu;
    set_text(g.confirm, "labHeader", question);
    open_page(g.confirm, "btnNo");
}

// Back from a page: where it was opened from
void page_back() {
    xui::Scene* m = g.menu;
    if (m == g.confirm) { ask = AskNone; open_page(askFrom, 0); if (list_of(askFrom)) fill_list(askFrom); return; }
    if (m == g.saves) { show_menu(g.game, "btnManageGameFiles"); return; }
    g.full = false;
    g.hud->play("FullToHalf");
    show_menu(g.main, m == g.recent ? "btnBlank1" : m == g.downloads ? "btnQuickChat" : "ctlGamerCard");
}

void open_guide() {
    g.state = Opening;
    g.hud->play("ClosedToHalf");
    show_tab(0, false);
    session_text();
    update_clock();
    show_port();
    { static const char* dbg = getenv("XBCOMPAT_GUIDE_DEBUG"); if (!dbg || !strstr(dbg, "nomask")) xinput_guide_mask(true); }
    ds_set_duck(0.3f);
    play(SndOpen);
    xlog("Guide: open (pad %d)", g.port + 1);
}

void close_guide() {
    g.state = Closing;
    g.hud->play(g.full ? "FullToClosed" : "HalfToClosed");
    g.full = false;
    play(SndClose);
    xinput_guide_mask(false);
    ds_set_duck(1.0f);
}

void leave() {
    g.state = Leaving;
    g.leaveAt = now_s() + 0.15;  // let the audio fade first
    ds_set_duck(0.0f);
}

// A on a control; true when it did something (the select sound), false for
// a row with nothing behind it yet (the inactive sound).
bool press(const char* id) {
    if (!strcmp(id, "ctlGamerCard")) {
        // the page is the width of the Blades' screen: the frame opens out
        game_page();
        g.full = true;
        g.hud->play("HalfToFull");
        show_menu(g.game, "btnViewAchievements");
        if (xui::Node* pg = g.game->find("page")) pg->play("Open");
    }
    else if (!strcmp(id, "btnViewAchievements")) return false;  // Controller Configuration: not here yet
    else if (!strcmp(id, "btnManageGameFiles")) {  // Game Storage: Frolick's Saved Games
        if (!g.saves) return false;
        scan_saves(); lSaves.count = nsaves; lSaves.sel = lSaves.scroll = 0;
        open_page(g.saves, 0);
    }
    else if (!strcmp(id, "btnOptions")) leave();  // Exit Game
    else if (!strcmp(id, "btnBlank1")) {  // Quick Launch: Frolick's Recent Games
        if (!g.recent) return false;
        scan_recent(); lRecent.count = ngames; lRecent.sel = lRecent.scroll = 0;
        open_page(g.recent, 0);
    }
    else if (!strcmp(id, "btnQuickChat")) {  // Downloads: Frolick's Active Downloads
        if (!g.downloads) return false;
        load_downloads(); lDownloads.sel = 0;
        open_page(g.downloads, 0);
    }
    else if (!strncmp(id, "list_items_", 11)) {
        List* l = list_of(g.menu);
        if (!l || l->sel >= l->count) return false;
        if (g.menu == g.recent) {
            if (!guide_launch) return false;
            xlog("Guide: Quick Launch %s", games[l->sel].name);
            leave();
            g.launchDir = games[l->sel].ntdir;
        }
        else if (g.menu == g.downloads) {
            if (!dl_press(l->sel - dl_off())) return false;
            fill_list(g.downloads);
        }
        else return false;  // a save: Y deletes it
    }
    else if (!strcmp(id, "btnYes") || !strcmp(id, "btnNo")) {
        bool yes = id[3] == 'Y';
        if (yes && ask == AskDeleteSave && lSaves.sel < nsaves) {
            xlog("Guide: delete save %s", saves[lSaves.sel].dir);
            remove_tree(saves[lSaves.sel].dir);
            scan_saves(); lSaves.count = nsaves;
            game_page();  // its saved games count
        }
        if (yes && ask == AskRemoveDownload) {
            // the item asked about, wherever it is now
            load_downloads();
            for (int i = 0; i < dl_count(); i++) {
                DlItem it = dl_item(i);
                const char* did = it.q >= 0 ? dls[it.q].id : it.s >= 0 ? streams[it.s].id : "";
                if (!strcmp(did, askId)) { dl_remove_item(i); break; }
            }
        }
        page_back();
    }
    else if (!strcmp(id, "btnOnlineStatus")) { settings_text(); show_menu(g.volume, "sliderVolume"); }
    else if (!strcmp(id, "btnVoice")) { settings_text(); show_menu(g.vibration, "chkVibration"); }
    else if (!strcmp(id, "chkVibration")) {
        g.vibrationOn = !g.vibrationOn;
        apply_settings(); write_settings(); settings_text();
    }
    return true;
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
    case Open: {
        if (g.state == Opening && !g.hud->playing) g.state = Open;
        bool home = g.menu == g.main || g.menu == g.settings;
        if ((down & GUIDE_B) && !home && !toggle) {
            play(SndBack);
            if (g.menu == g.game || list_of(g.menu) || g.menu == g.confirm) page_back();
            else show_menu(g.settings, g.menu == g.volume ? "btnOnlineStatus" : "btnVoice");
            break;
        }
        if (toggle || (down & GUIDE_B)) { xlog("Guide: close (buttons %#x down %#x)", b, down); close_guide(); break; }
        if ((b & (GUIDE_LEFT | GUIDE_RIGHT)) && g.menu == g.volume) {
            if ((down & (GUIDE_LEFT | GUIDE_RIGHT)) || t >= g.repeatAt) {
                int v = g.volumeLevel + ((b & GUIDE_RIGHT) ? 1 : -1);
                v = v < 0 ? 0 : v > 10 ? 10 : v;
                if (v != g.volumeLevel) {
                    g.volumeLevel = v;
                    apply_settings(); write_settings(); settings_text();
                    play(SndFocus);
                }
                g.repeatAt = t + ((down & (GUIDE_LEFT | GUIDE_RIGHT)) ? 0.4 : 0.12);
            }
            break;
        }
        if (home && (down & (GUIDE_LT | GUIDE_RT | GUIDE_LEFT | GUIDE_RIGHT))) {
            int t = (down & (GUIDE_RT | GUIDE_RIGHT)) ? 1 : 0;
            if (t != g.tab) { play(SndTab); show_tab(t, true); }
            break;
        }
        if ((down & GUIDE_Y) && home) { leave(); break; }
        if ((down & GUIDE_Y) && g.menu == g.saves && lSaves.sel < nsaves) {  // Delete, as Frolick's Y
            play(SndSelect);
            char q[160]; snprintf(q, sizeof q, "Delete %s?", saves[lSaves.sel].name);
            ask_confirm(AskDeleteSave, q);
            break;
        }
        if ((down & GUIDE_X) && g.menu == g.downloads && dl_count() && lDownloads.sel - dl_off() >= 0) {
            play(SndSelect);
            DlItem it = dl_item(lDownloads.sel - dl_off());
            const char* name = it.q >= 0 ? dls[it.q].name : it.s >= 0 ? streams[it.s].name : "";
            snprintf(askId, sizeof askId, "%s", it.q >= 0 ? dls[it.q].id : it.s >= 0 ? streams[it.s].id : "");
            // a Streaming Download's game goes with it (Frolick's Remove)
            char q[200]; snprintf(q, sizeof q, it.s >= 0 ? "Remove %s and delete its game?" : "Remove %s from Downloads?", name);
            ask_confirm(AskRemoveDownload, q);
            break;
        }
        if (down & GUIDE_A) {
            if (g.menu->focus && g.menu != g.volume) {
                const char* id = g.menu->focus->id();
                g.menu->press();
                play(press(id) ? SndSelect : SndInactive);
            }
            break;
        }
        if (b & (GUIDE_UP | GUIDE_DOWN)) {
            if ((down & (GUIDE_UP | GUIDE_DOWN)) || t >= g.repeatAt) {
                bool moved = list_of(g.menu) ? list_move((b & GUIDE_UP) ? -1 : 1)
                                             : g.menu->navigate((b & GUIDE_UP) ? "up" : "down");
                if (moved) play(SndFocus);
                g.repeatAt = t + ((down & (GUIDE_UP | GUIDE_DOWN)) ? 0.4 : 0.12);
            }
        }
        break;
    }
    case Closing:
        if (!g.hud->playing) g.state = Closed;
        break;
    case Leaving:
        if (t >= g.leaveAt) {
            if (g.launchDir && guide_launch) guide_launch(g.launchDir, "default.xbe");
            reset_request_always("Guide");
        }
        break;
    case Off:
        break;
    }
}

}  // namespace

namespace {
void advance_tree(xui::Node* n, double frames) {
    if (n->timeline && n->playing) n->advance(frames);
    if (n->visual) advance_tree(n->visual, frames);
    for (int i = 0; i < n->children.n; i++) advance_tree(n->children[i], frames);
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
    if (g.menu == g.downloads && t - g.listedAt >= 1) {  // the helper's progress, once a second
        g.listedAt = t;
        load_downloads();
        fill_list(g.downloads);
    }
    { static const char* dbg = getenv("XBCOMPAT_GUIDE_DEBUG"); if (dbg && strstr(dbg, "nodraw")) return false; }
    g.frame->update(dt);
    g.menu->update(dt);
    g.tabs->update(dt);
    if (g.card) {
        // The game box lights with GFWL's own card highlight (baseMetaPane,
        // btn_GamercardHighlight) while the menu's ctlGamerCard has the focus.
        xui::Node* f = g.menu == g.main ? g.main->focus : 0;
        xui::Node* lit = f && !strcmp(f->id(), "ctlGamerCard") ? g.card->find("baseMetaPane") : 0;
        if (lit != g.card->focus) g.card->setFocus(lit);
        // Only the highlight runs: the card's own timelines (Normal, Macro,
        // Mini) would move its lines.
        if (xui::Node* b = g.card->find("baseMetaPane")) advance_tree(b, dt * 30);
    }
    session_text();
    (void)w; (void)h;
    return true;
}

namespace {

// The box of a prompt's button disc in the prompt's own frame: the button
// image in its visual, whose disc fills the middle 3/4.
bool legend_disc(xui::Node* n, float ox, float oy, float* x0, float* y0, float* x1, float* y1) {
    for (int i = 0; i < n->children.n; i++) {
        xui::Node* c = n->children[i];
        xui::Vec3 z = {0, 0, 0}, one = {1, 1, 1};
        xui::Vec3 p = xui::propV(c->props, "Position", z);
        if (c->cls && !strcmp(c->cls->name, "XuiImage") && strstr(xui::propS(c->props, "ImagePath"), "utton")) {
            xui::Vec3 sc = xui::propV(c->props, "Scale", one), pv = xui::propV(c->props, "Pivot", z);
            float w = c->width() * sc.x, h = c->height() * sc.y;
            float l = ox + p.x + pv.x * (1 - sc.x), t = oy + p.y + pv.y * (1 - sc.y);
            *x0 = l + w / 8; *x1 = l + w * 7 / 8; *y0 = t + h / 8; *y1 = t + h * 7 / 8;
            return true;
        }
        if (legend_disc(c, ox + p.x, oy + p.y, x0, y0, x1, y1)) return true;
    }
    return false;
}

// ---- SD layout ------------------------------------------------------------
// The scene's timelines own the frame's positions and widths, so an offset
// of ours is kept apart from them: the base is what the scene last wrote,
// and the offset is re-applied over it every frame.
struct Track { xui::Node* n; char key; float lastSet, lastBase; };
static Track tracks[160]; static int ntracks;
static Track* track_of(xui::Node* n, char key) {
    for (int i = 0; i < ntracks; i++) if (tracks[i].n == n && tracks[i].key == key) return &tracks[i];
    if (ntracks == (int)(sizeof tracks / sizeof tracks[0])) return 0;
    Track* t = &tracks[ntracks++]; t->n = n; t->key = key; t->lastSet = t->lastBase = -1e30f;
    return t;
}
// key: 'x' Position.x, 'y' Position.y, 'w' Width, 's' Scale.x
static float tracked_prop(xui::Node* n, char key) {
    xui::Vec3 z = {0, 0, 0}, one = {1, 1, 1};
    if (key == 'x') return xui::propV(n->props, "Position", z).x;
    if (key == 'y') return xui::propV(n->props, "Position", z).y;
    if (key == 's') return xui::propV(n->props, "Scale", one).x;
    return n->width();
}
static void set_vec(xui::Node* n, const char* key, xui::Vec3 v) {
    xui::Value& p = n->props.set(key); p.free(); p.type = xui::TVector; p.v = v;
}
// the value the scene gave the property, under our offset
static float base_of(xui::Node* n, char key) {
    Track* t = track_of(n, key); float cur = tracked_prop(n, key);
    return t && cur == t->lastSet ? t->lastBase : cur;
}
// sets the property to the scene's value plus d
static void offset(xui::Node* n, char key, float d) {
    Track* t = track_of(n, key);
    if (!t) return;
    float base = base_of(n, key), v = base + d;
    xui::Vec3 z = {0, 0, 0}, one = {1, 1, 1};
    if (key == 'x' || key == 'y') { xui::Vec3 p = xui::propV(n->props, "Position", z); (key == 'x' ? p.x : p.y) = v; set_vec(n, "Position", p); }
    else if (key == 's') { xui::Vec3 p = xui::propV(n->props, "Scale", one); p.x = v; set_vec(n, "Scale", p); }
    else { xui::Value& w = n->props.set("Width"); w.free(); w.type = xui::TFloat; w.f = v; }
    t->lastSet = v; t->lastBase = base;
}

// The menus' rows: the highlight bars and rules that span the panel's face
// (about 615 of the scene's units, the controls themselves 420).
static void collect_rows(xui::Node* n, bool underRow) {
    float w = n->width();
    bool row = n->cls && (!strcmp(n->cls->name, "XuiFigure") || !strcmp(n->cls->name, "XuiGroup")) && w >= 600 && w <= 640;
    if (row && !underRow && nrows < 64) rows[nrows++] = n;
    if (n->visual) collect_rows(n->visual, underRow || row);
    for (int i = 0; i < n->children.n; i++) collect_rows(n->children[i], underRow || row);
}

// Narrows the half-open frame by n canvas units each side: the blades,
// the panel and its reflection, the close box and the gamer tag move in,
// and the menus' rows are squeezed about their middle, so the content
// column stays centred in the panel.  The clock stays put: GFWL has it well
// in from the edge, and narrowed it then sits by the blade as in the full
// view (sokky: "too far in the center").
static void narrow_frame(float n) {
    static const struct { const char* id; float dx, dw; } adj[] = {
        { "leftBlade", 1, 0 }, { "reflectLeft", 1, 0 },
        { "rightBlade", -1, 0 }, { "reflectRight", -1, 0 }, { "GamerTag", -1, 0 },
        { "CloseX", -1, 0 },
        { "backpanel", 1, -2 }, { "reflectMid", 1, -2 },
    };
    for (unsigned i = 0; i < sizeof adj / sizeof adj[0]; i++) {
        xui::Node* nd = g.frame->find(adj[i].id);
        if (!nd) continue;
        offset(nd, 'x', n * adj[i].dx);
        if (adj[i].dw) offset(nd, 'w', n * adj[i].dw);
    }
    xui::Node* host = g.frame->find("AppHostElementId");
    xui::Vec3 one = {1, 1, 1};
    float hs = host ? xui::propV(host->props, "Scale", one).x : 0.85f;
    if (hs <= 0) hs = 0.85f;
    for (int i = 0; i < nrows; i++) {
        xui::Node* r = rows[i];
        float w = base_of(r, 'w');
        xui::Vec3 pv = { w / 2, r->height() / 2, 0 };
        set_vec(r, "Pivot", pv);
        offset(r, 's', w > 0 ? -2 * n / hs / w : 0);
    }
}

// Keeps the frame's button prompts' discs inside the title-safe box (canvas
// rect l,t-r,b).  The prompts on each side move together, just enough for
// all of them to fit, so a stack (Back over Select) keeps its spacing;
// their labels run inward from the disc.  (They don't follow the narrowed
// panel's edges: GFWL sets them in from the edges by about as much as the
// narrowing.)
void fit_legends(float l, float t, float r, float b) {
    // (X is never drawn.  A prompt a page hides still counts, so the
    // others don't jump between pages.)
    static const char* ids[] = { "Legend_A", "Legend_B", "Legend_Y" };
    xui::Node* nd[3]; float bx[3], by[3]; int side[3];
    float dx[2] = {0, 0}, dy[2] = {0, 0};
    for (int i = 0; i < 3; i++) {
        nd[i] = g.frame->find(ids[i]);
        if (!nd[i]) continue;
        float x0, y0, x1, y1;
        if (!legend_disc(nd[i]->visual ? nd[i]->visual : nd[i], 0, 0, &x0, &y0, &x1, &y1)) { nd[i] = 0; continue; }
        bx[i] = base_of(nd[i], 'x'); by[i] = base_of(nd[i], 'y');
        // the disc on the canvas at the scene's position
        float sx0 = bx[i] + x0, sx1 = bx[i] + x1, sy0 = by[i] + y0, sy1 = by[i] + y1;
        int sd = side[i] = sx0 + sx1 > l + r;
        if (sx1 > r && r - sx1 < dx[sd]) dx[sd] = r - sx1;
        if (sx0 < l && l - sx0 > dx[sd]) dx[sd] = l - sx0;
        if (sy1 > b && b - sy1 < dy[sd]) dy[sd] = b - sy1;
        if (sy0 < t && t - sy0 > dy[sd]) dy[sd] = t - sy0;
    }
    for (int i = 0; i < 3; i++) {
        if (!nd[i]) continue;
        offset(nd[i], 'x', dx[side[i]]);
        offset(nd[i], 'y', dy[side[i]]);
    }
}

}  // namespace

// The panel's face (the frame's backpanel) in the menus' units, given the
// menu's origin and scale on the canvas.
static bool panel_span(float hx, float hs, float* l, float* r) {
    xui::Node* bp = g.frame->find("backpanel");
    if (!bp || hs <= 0) return false;
    float bx = 0, bs = 1;
    for (xui::Node* p = bp; p; p = p->parent) {
        xui::Vec3 z = {0, 0, 0}, one = {1, 1, 1};
        xui::Vec3 v = xui::propV(p->props, "Position", z), sc = xui::propV(p->props, "Scale", one);
        xui::Vec3 pv = xui::propV(p->props, "Pivot", z);
        bx = v.x + pv.x + sc.x * (bx - pv.x);
        bs *= sc.x;
    }
    *l = (bx - hx) / hs; *r = (bx + bp->width() * bs - hx) / hs;
    return true;
}

void guide_draw(int w, int h) {
    // The frame is a 4:3 canvas (800x600); fit the height and stretch x to
    // the window the way the game image is stretched.
    float cw = g.frame->root->width(), ch = g.frame->root->height();
    float s = h / ch;
    float kx = (w / cw) / s;
    { static const char* dbg = getenv("XBCOMPAT_GUIDE_DEBUG");
      if (dbg && strstr(dbg, "glerr")) { GLenum e = glGetError(); if (e) xlog("Guide: GL error %#x before drawing", e); } }
    // TV layout.  GFWL's frame was drawn for a PC window, its half-open
    // panel 80% of the screen's width.  On a TV it is narrowed by layout
    // (the text stays its size): the blades, panel and prompts move in by
    // `narrow` canvas units each side, easing back to the authored layout
    // as the frame expands, the full view filling the screen.  The button
    // prompts are moved in wherever they'd leave the title-safe box (80%).
    // XBCOMPAT_GUIDE_NARROW sets the amount (0 turns it off).
    static float narrow = getenv("XBCOMPAT_GUIDE_NARROW") ? (float)atof(getenv("XBCOMPAT_GUIDE_NARROW")) : 60;
    float t = 0;
    if (xui::Node* lb = g.frame->find("leftBlade")) {
        t = (82 - base_of(lb, 'x')) / 81;  // half 82, full 1
        if (t < 0) t = 0; if (t > 1) t = 1;
    }
    float n = narrow * (1 - t);
    // On an SD screen the scenes' text, sized for a PC window, comes out a
    // little small from the couch: draw it larger, each label growing about
    // its own box, to the size of Frolick's rows at 480 lines (their
    // capitals 12-13 px to the authored size's 10-11).
    // XBCOMPAT_GUIDE_TEXT sets the factor (1 keeps the authored size).
    static float zoom = getenv("XBCOMPAT_GUIDE_TEXT") ? (float)atof(getenv("XBCOMPAT_GUIDE_TEXT")) : 1.2f;
    g.r->textZoom = h <= 576 && zoom > 0 ? zoom : 1;
    // Frolick's page is sized for Frolick's screen already (its text at
    // Frolick's size at this scale)
    if (is_page(g.menu)) g.r->textZoom = 1;
    narrow_frame(n);
    fit_legends(cw * 0.1f, ch * 0.1f, cw * 0.9f, ch * 0.9f);
    float fx = 0, fy = 0;
    g.be.begin(w, h, kx);
    g.r->draw(g.frame, fx, fy, s);
    // The menu sits in the frame's AppHostElementId and fades with it.
    xui::Node* host = g.frame->find("AppHostElementId");
    // Its origin and scale on the canvas (the slot is at 0.85).
    float hx = 0, hy = 0, hs = 1, op = 1; bool shown = true;
    for (xui::Node* p = host; p; p = p->parent) {
        xui::Vec3 z = {0, 0, 0}, one = {1, 1, 1};
        xui::Vec3 v = xui::propV(p->props, "Position", z);
        xui::Vec3 sc = xui::propV(p->props, "Scale", one);
        xui::Vec3 pv = xui::propV(p->props, "Pivot", z);
        hx = v.x + pv.x + sc.x * (hx - pv.x);
        hy = v.y + pv.y + sc.y * (hy - pv.y);
        hs *= sc.x;
        shown = shown && xui::propB(p->props, "Show", true);
        op *= xui::propF(p->props, "Opacity", 1);
    }
    if (host && shown && op > 0.001f) {
        xui::Value& o = g.menu->root->props.set("Opacity"); o.free(); o.type = xui::TFloat; o.f = op;
        // Frolick's list pages with Frolick's own baseline (the font's ascent):
        // their two-line rows and bars are packed to it
        float ae = g.r->ascentEm;
        if (list_of(g.menu) || g.menu == g.confirm) g.r->ascentEm = 0;
        g.r->draw(g.menu, fx + hx * s, fy + hy * s, s * hs);
        g.r->ascentEm = ae;
        xui::Node* slot = g.menu == g.main && g.card ? g.main->find("ctlGamerCard") : 0;
        if (slot && xui::propB(slot->props, "Show", true)) {
            xui::Vec3 z = {0, 0, 0};
            xui::Vec3 v = xui::propV(slot->props, "Position", z);
            xui::Value& co = g.card->root->props.set("Opacity"); co.free(); co.type = xui::TFloat; co.f = op;
            float ms = s * hs;
            float tz = g.r->textZoom;
            if (g.r->textZoom > 1.15f) g.r->textZoom = 1.15f;
            g.r->draw(g.card, fx + hx * s + v.x * ms, fy + hy * s + v.y * ms, ms);
            g.r->textZoom = tz;
        }
        // The home's tab strip over the page's top, spanning the rows (the
        // panel's face, FACE_X to FACE_X + FACE_W, squeezed by the SD
        // narrowing); the full view moves the menu's left past the panel's
        // edge, and the strip starts at the edge.
        if (g.menu == g.main || g.menu == g.settings) {
            float k = FACE_W / STRIP_W, ms = s * hs;
            float in = hs > 0 ? n / hs : 0;
            float left = FACE_X + in, right = FACE_X + FACE_W - in;
            float pl, pr;
            if (panel_span(hx, hs, &pl, &pr) && pl > left) left = pl;
            float width = right - left;
            if (xui::Node* clip = g.tabs->find("tabClip")) {
                xui::Value& v = clip->props.set("Width"); v.free(); v.type = xui::TFloat; v.f = width / k;
            }
            // The strip shows `width` of its 855 units: the slant moves to
            // the middle so both tabs keep half (the bars run on past the
            // edges), and each title is centred in its tab.
            if (g.strip) {
                float W = width / k, d = STRIP_X + W / 2 - SLANT_X, sx = SLANT_X + d;
                for (int i = 0; i < g.strip->children.n; i++) {
                    xui::Node* c = g.strip->children[i];
                    const char* id = c->id();
                    float x0 = 0, w = 0;
                    if (!strcmp(id, "tab_1_header")) { x0 = STRIP_X; w = sx - SLANT_GAP - STRIP_X; }
                    else if (!strcmp(id, "tab_2_header")) { x0 = sx + SLANT_GAP; w = STRIP_X + W - x0; }
                    else { offset(c, 'x', d); continue; }
                    offset(c, 'x', x0 - base_of(c, 'x'));
                    xui::Value& v = c->props.set("Width"); v.free(); v.type = xui::TFloat; v.f = w;
                }
            }
            xui::Value& to = g.tabs->root->props.set("Opacity"); to.free(); to.type = xui::TFloat; to.f = op;
            g.r->draw(g.tabs, fx + hx * s + (left - STRIP_X * k) * ms, fy + hy * s + (TAB_Y - STRIP_Y * k) * ms, ms * k);
        }
    }
    g.be.end();
    // Test aid: XBCOMPAT_GUIDE_SHOT=file.ppm saves the window (game and
    // Guide) after the Guide's Nth drawn frame (XBCOMPAT_GUIDE_SHOT_AT, 40).
    static int drawn;
    const char* shot = getenv("XBCOMPAT_GUIDE_SHOT");
    const char* at = getenv("XBCOMPAT_GUIDE_SHOT_AT");
    if (shot && ++drawn == (at ? atoi(at) : 40)) {
        GLint prog = -1, fb = -1, rb = -1; GLenum err = glGetError();
        glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
        glGetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &fb);
        glGetIntegerv(GL_READ_BUFFER, &rb);
        xlog("Guide: shot state %d playing %d err %#x program %d fb %d readbuf %#x size %dx%d", g.state, g.hud->playing, err, prog, fb, rb, w, h);
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
