// guideview: renders one Guide scene (or the whole Guide) to a .ppm with the
// same XUI runtime and GL backend xbcompat uses, for side-by-side checks
// against the GFWL engine's own render of the scene.
//
//   guideview DIR SCENE.xui OUT.ppm [options]
//     --skin F      skin scene (default hudskin.xui)
//     --play NAME   play a named frame first (runs to its end; with
//                   --guide, on the Guide's frame, e.g. HalfToFull)
//     --frames N    frames to step at 30 fps before the shot (default 60)
//     --scale S --at X,Y   where to draw the scene (default 1, 0,0)
//     --size WxH    window (default 1280x720)
//     --bg RRGGBB   clear colour (default 1a1c20, the GFWL host's)
//     --focus ID    focus this control
//     --guide       the whole Guide as xbcompat draws it (frame + main)
//     --page P      ... with page P open: settings, volume, vibration
//     --dump        print the frame's tree (id, class, size, position, scale, show)
//
// make build/guideview.  Builds against src/guide/guide.cpp (included) with
// stand-ins for the xbcompat calls it makes.
#include <stdarg.h>
#include "../../src/guide/guide.cpp"

extern "C" {
ULONG xbe_title_id(void) { return 0x4d5300c8; }
const char* xbe_title_name(void) { return getenv("GUIDEVIEW_TITLE") ? getenv("GUIDEVIEW_TITLE") : "Marble Blast"; }
unsigned xinput_guide_read(int* port) { *port = 0; return 0; }
void xinput_guide_mask(bool) {}
void ds_set_duck(float) {}
void ds_set_volume(float) {}
void ds_play_ui(const float*, int) {}
unsigned xinput_guide_ports(void) { return 1; }
void xinput_guide_vibration(bool) {}
const char* fs_hdd_root(void) { return "/tmp"; }
void reset_request_always(const char*) {}
void xlog(const char* fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr);
}
}

static void shot(const char* out, int w, int h) {
    unsigned char* px = new unsigned char[w * h * 3];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE* f = fopen(out, "wb");
    if (!f) { perror(out); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) fwrite(px + y * w * 3, 1, w * 3, f);
    fclose(f);
    delete[] px;
}

static void dump_tree(xui::Node* n, int depth) {
    xui::Vec3 z = {0, 0, 0}, one = {1, 1, 1};
    xui::Vec3 p = xui::propV(n->props, "Position", z), sc = xui::propV(n->props, "Scale", one);
    float op = xui::propF(n->props, "Opacity", 1);
    printf("%*s%s [%s] %gx%g at %g,%g scale %g,%g%s op %g\n", depth * 2, "", n->id(), n->cls ? n->cls->name : "?",
           n->width(), n->height(), p.x, p.y, sc.x, sc.y, xui::propB(n->props, "Show", true) ? "" : " hidden", op);
    if (n->visual) dump_tree(n->visual, depth + 1);
    for (int i = 0; i < n->children.n; i++) dump_tree(n->children[i], depth + 1);
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: guideview DIR SCENE.xui OUT.ppm [options]\n"); return 2; }
    const char *dir = argv[1], *sceneName = argv[2], *out = argv[3], *skinName = "hudskin.xui", *play = 0, *focus = 0;
    int frames = 60, W = 1280, H = 720; float scale = 1, ax = 0, ay = 0; unsigned bg = 0x1a1c20;
    bool whole = false, dump = false; const char* page = 0;
    for (int i = 4; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--skin")) skinName = v, i++;
        else if (!strcmp(a, "--play")) play = v, i++;
        else if (!strcmp(a, "--frames")) frames = atoi(v), i++;
        else if (!strcmp(a, "--scale")) scale = (float)atof(v), i++;
        else if (!strcmp(a, "--at")) { sscanf(v, "%f,%f", &ax, &ay); i++; }
        else if (!strcmp(a, "--size")) { sscanf(v, "%dx%d", &W, &H); i++; }
        else if (!strcmp(a, "--bg")) bg = (unsigned)strtoul(v, 0, 16), i++;
        else if (!strcmp(a, "--focus")) focus = v, i++;
        else if (!strcmp(a, "--guide")) whole = true;
        else if (!strcmp(a, "--page")) whole = true, page = v, i++;
        else if (!strcmp(a, "--dump")) dump = true;
    }
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_Window* win = SDL_CreateWindow("guideview", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win || !SDL_GL_CreateContext(win)) { fprintf(stderr, "no GL: %s\n", SDL_GetError()); return 1; }
    glViewport(0, 0, W, H);
    float br = ((bg >> 16) & 255) / 255.0f, bgc = ((bg >> 8) & 255) / 255.0f, bb = (bg & 255) / 255.0f;
    if (whole) {
        setenv("XBCOMPAT_GUIDE_DIR", dir, 1);
        setenv("XBCOMPAT_GUIDE_SETTINGS", "/dev/null", 1);
        guide_frame(W, H);          // loads
        open_guide();
        if (page && !strcmp(page, "settings")) show_menu(g.settings, "btnOnlineStatus");
        if (page && !strcmp(page, "volume")) { settings_text(); show_menu(g.volume, "sliderVolume"); }
        if (page && !strcmp(page, "vibration")) { settings_text(); show_menu(g.vibration, "chkVibration"); }
        if (focus) g.menu->setFocus(g.menu->find(focus));
        if (play) for (int i = 0; i < 6; i++) { g.lastTime = now_s() - 1.0 / 30; guide_frame(W, H); }
        if (play) g.hud->play(play);  // the frame's state, e.g. HalfToFull
        for (int i = 0; i < frames; i++) {
            g.lastTime = now_s() - 1.0 / 30;  // a steady 30 fps whatever the host does
            guide_frame(W, H);
        }
        glClearColor(br, bgc, bb, 1); glClear(GL_COLOR_BUFFER_BIT);
        guide_draw(W, H);
        shot(out, W, H);
        if (dump) { dump_tree(g.frame->root, 0); printf("== menu\n"); dump_tree(g.menu->root, 0); }
        return 0;
    }
    snprintf(g.res.dir, sizeof g.res.dir, "%s", dir);
    char err[256];
    xui::Element* skin = g.res.scene(skinName, err, sizeof err);
    xui::Element* el = g.res.scene(sceneName, err, sizeof err);
    if (!el) { fprintf(stderr, "%s: %s\n", sceneName, err); return 1; }
    xui::Renderer r(&g.be, &g.res);
    r.setFont("font.ttf");
    r.defaultTextColor = 0xff000000;
    r.ascentEm = 1.12f;
    xui::Scene sc(el, skin);
    if (focus) sc.setFocus(sc.find(focus));
    if (play) { xui::Node* n = sc.root; n->play(play);
        for (int i = 0; i < n->children.n; i++) n->children[i]->play(play); }
    for (int i = 0; i < frames; i++) sc.update(1.0 / 30);
    glClearColor(br, bgc, bb, 1); glClear(GL_COLOR_BUFFER_BIT);
    g.be.begin(W, H, 1);
    r.draw(&sc, ax, ay, scale);
    g.be.end();
    shot(out, W, H);
    return 0;
}
