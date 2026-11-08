// --view (see viewout.h)
#include "viewout.h"
#include <cstdlib>
#include <cstring>

#ifdef HAVE_SDL_VIDEO
#include <SDL.h>
#endif

bool ViewOut::Open(const std::string& spec)
{
    if (spec.rfind("dump:", 0) == 0)
    {
        dump = fopen(spec.c_str() + 5, "wb");
        if (!dump) { perror(spec.c_str() + 5); return false; }
        if (const char* e = getenv("MIRROR_TVDUMPEVERY")) dumpEvery = atoi(e) ? atoi(e) : 60;
        dumpBuf.assign(w * h, 0);
        Scale();
        fprintf(stderr, "view: %ux%u pictures to %s\n", w, h, spec.c_str() + 5);
        return true;
    }
#ifdef HAVE_SDL_VIDEO
    if (spec == "sdl")
    {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   // SIGTERM/SIGINT are mirrorrun's
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0)
        {
            fprintf(stderr, "view: SDL: %s\n", SDL_GetError());
            return false;
        }
        if (const char* db = getenv("SDL_GAMECONTROLLERCONFIG_FILE")) SDL_GameControllerAddMappingsFromFile(db);
        SDL_DisplayMode dm;
        // BananaOS's panels together (SDL_BANANA_PANELS=stack) make one 640x960 display
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0 && getenv("SDL_BANANA_PANELS"))
            w = dm.w, h = dm.h;
        else if (const char* sz = getenv("MIRROR_VIEW_SIZE"))
            sscanf(sz, "%ux%u", &w, &h);
        SDL_Window* win = SDL_CreateWindow("Sion Link", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
                                           getenv("SDL_BANANA_PANELS") ? SDL_WINDOW_FULLSCREEN : 0);
        if (!win)
        {
            fprintf(stderr, "view: window: %s\n", SDL_GetError());
            return false;
        }
        window = win;
        SDL_GetWindowSize(win, (int*)&w, (int*)&h);
        SDL_ShowCursor(SDL_DISABLE);
        Scale();
        fprintf(stderr, "view: SDL %s, %ux%u\n", SDL_GetCurrentVideoDriver(), w, h);
        return true;
    }
#endif
    fprintf(stderr, "view: unknown output %s (sdl or dump:FILE)\n", spec.c_str());
    return false;
}

// Each screen gets as much of its half (top or bottom) as fits at 4:3, centred.
void ViewOut::Scale()
{
    uint32_t hh = h / 2;
    sw = w, sh = w * 3 / 4;
    if (sh > hh) sh = hh, sw = hh * 4 / 3;
    sx0 = (w - sw) / 2;
    sy0[0] = (hh - sh) / 2;
    sy0[1] = hh + (hh - sh) / 2;
    xs.resize(sw);
    ys.resize(sh);
    for (uint32_t x = 0; x < sw; x++) xs[x] = x * 256 / sw;
    for (uint32_t y = 0; y < sh; y++) ys[y] = y * 192 / sh;
}

void ViewOut::Compose(uint32_t* dst, uint32_t pitch, const uint32_t* top, const uint32_t* bottom)
{
    uint32_t stride = pitch / 4;
    for (uint32_t y = 0; y < h; y++) memset(dst + y * stride, 0, w * 4);
    const uint32_t* src[2] = {top, bottom};
    for (int s = 0; s < 2; s++)
    {
        uint32_t lastY = ~0u;
        for (uint32_t y = 0; y < sh; y++)
        {
            uint32_t* row = dst + (sy0[s] + y) * stride + sx0;
            if (ys[y] == lastY)
            {
                memcpy(row, row - stride, sw * 4);   // the row above shows the same source row
                continue;
            }
            lastY = ys[y];
            const uint32_t* in = src[s] + ys[y] * 256;
            for (uint32_t x = 0; x < sw; x++) row[x] = in[xs[x]] | 0xFF000000u;
        }
    }
}

void ViewOut::Show(const uint32_t* top, const uint32_t* bottom)
{
    if (dump)
    {
        if (frames++ % dumpEvery == 0)
        {
            Compose(dumpBuf.data(), w * 4, top, bottom);
            fwrite(dumpBuf.data(), 4, dumpBuf.size(), dump);
            fflush(dump);
        }
        return;
    }
#ifdef HAVE_SDL_VIDEO
    SDL_Window* win = (SDL_Window*)window;
    if (!win) return;
    SDL_Surface* s = SDL_GetWindowSurface(win);
    if (!s) return;
    if (s->format->format == SDL_PIXELFORMAT_XRGB8888 || s->format->format == SDL_PIXELFORMAT_ARGB8888)
    {
        if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
        Compose((uint32_t*)s->pixels, s->pitch, top, bottom);
        if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
    }
    else
    {
        // another format: compose in ours and let SDL convert
        if (dumpBuf.size() != w * h) dumpBuf.assign(w * h, 0);
        Compose(dumpBuf.data(), w * 4, top, bottom);
        SDL_Surface* from = SDL_CreateRGBSurfaceWithFormatFrom(dumpBuf.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_XRGB8888);
        if (from) { SDL_BlitSurface(from, nullptr, s, nullptr); SDL_FreeSurface(from); }
    }
    SDL_UpdateWindowSurface(win);
#endif
}

#ifdef HAVE_SDL_VIDEO
// a point on the window, in the bottom screen: the stylus there
static bool OnBottom(uint32_t px, uint32_t py, uint32_t sx0, uint32_t sy0, uint32_t sw, uint32_t sh, uint16_t& x, uint16_t& y)
{
    if (px < sx0 || py < sy0 || px >= sx0 + sw || py >= sy0 + sh) return false;
    x = (px - sx0) * 256 / sw;
    y = (py - sy0) * 192 / sh;
    return true;
}
#endif

void ViewOut::Poll(uint16_t& outHeld, bool& outTouch, uint16_t& outX, uint16_t& outY)
{
#ifdef HAVE_SDL_VIDEO
    if (window)
    {
        enum { A = 1, B = 2, SELECT = 4, START = 8, RIGHT = 16, LEFT = 32, UP = 64, DOWN = 128, R = 256, L = 512, X = 1024, Y = 2048 };
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
            case SDL_QUIT: quit = true; break;
            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which))
                {
                    pads.push_back(c);
                    fprintf(stderr, "view: pad %s\n", SDL_GameControllerName(c));
                }
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE) quit = true;
                break;
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
                touching = OnBottom(e.tfinger.x * w, e.tfinger.y * h, sx0, sy0[1], sw, sh, tx, ty);
                break;
            case SDL_FINGERUP: touching = false; break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT)
                    touching = OnBottom(e.button.x, e.button.y, sx0, sy0[1], sw, sh, tx, ty);
                break;
            case SDL_MOUSEMOTION:
                if (e.motion.which != SDL_TOUCH_MOUSEID && (e.motion.state & SDL_BUTTON_LMASK))
                    touching = OnBottom(e.motion.x, e.motion.y, sx0, sy0[1], sw, sh, tx, ty);
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) touching = false;
                break;
            }
        }
        uint16_t h16 = 0;
        for (void* p : pads)
        {
            SDL_GameController* c = (SDL_GameController*)p;
            auto b = [&](SDL_GameControllerButton btn) { return SDL_GameControllerGetButton(c, btn) != 0; };
            if (b(SDL_CONTROLLER_BUTTON_GUIDE)) quit = true;
            // by where they sit (SDL's names are an Xbox pad's)
            if (b(SDL_CONTROLLER_BUTTON_B)) h16 |= A;
            if (b(SDL_CONTROLLER_BUTTON_A)) h16 |= B;
            if (b(SDL_CONTROLLER_BUTTON_Y)) h16 |= X;
            if (b(SDL_CONTROLLER_BUTTON_X)) h16 |= Y;
            if (b(SDL_CONTROLLER_BUTTON_BACK)) h16 |= SELECT;
            if (b(SDL_CONTROLLER_BUTTON_START)) h16 |= START;
            if (b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) h16 |= L;
            if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) h16 |= R;
            if (b(SDL_CONTROLLER_BUTTON_DPAD_UP)) h16 |= UP;
            if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) h16 |= DOWN;
            if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) h16 |= LEFT;
            if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) h16 |= RIGHT;
            int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
            int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
            if (lx < -16000) h16 |= LEFT;
            if (lx > 16000) h16 |= RIGHT;
            if (ly < -16000) h16 |= UP;
            if (ly > 16000) h16 |= DOWN;
        }
        // a keyboard, for desktop tests: arrows, Z = B, X = A, A = Y, S = X, Q/W = L/R,
        // Enter = Start, Right Shift = Select
        const uint8_t* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_X]) h16 |= A;
        if (k[SDL_SCANCODE_Z]) h16 |= B;
        if (k[SDL_SCANCODE_S]) h16 |= X;
        if (k[SDL_SCANCODE_A]) h16 |= Y;
        if (k[SDL_SCANCODE_Q]) h16 |= L;
        if (k[SDL_SCANCODE_W]) h16 |= R;
        if (k[SDL_SCANCODE_RETURN]) h16 |= START;
        if (k[SDL_SCANCODE_RSHIFT]) h16 |= SELECT;
        if (k[SDL_SCANCODE_UP]) h16 |= UP;
        if (k[SDL_SCANCODE_DOWN]) h16 |= DOWN;
        if (k[SDL_SCANCODE_LEFT]) h16 |= LEFT;
        if (k[SDL_SCANCODE_RIGHT]) h16 |= RIGHT;
        held = h16;
    }
#endif
    outHeld = held;
    outTouch = touching;
    outX = tx;
    outY = ty;
}

void ViewOut::Close()
{
    if (dump) fclose(dump);
    dump = nullptr;
#ifdef HAVE_SDL_VIDEO
    for (void* p : pads) SDL_GameControllerClose((SDL_GameController*)p);
    pads.clear();
    if (window)
    {
        SDL_DestroyWindow((SDL_Window*)window);
        window = nullptr;
        SDL_Quit();
    }
#endif
}
