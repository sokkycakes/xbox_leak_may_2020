#include "pads.h"
#include "menu.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <pthread.h>
#include <string>
#include <unistd.h>

#ifdef HAVE_SDL
#include <SDL.h>

// the DS's buttons as libnds numbers them
enum { DS_A = 1 << 0, DS_B = 1 << 1, DS_SELECT = 1 << 2, DS_START = 1 << 3, DS_RIGHT = 1 << 4, DS_LEFT = 1 << 5,
       DS_UP = 1 << 6, DS_DOWN = 1 << 7, DS_R = 1 << 8, DS_L = 1 << 9, DS_X = 1 << 10, DS_Y = 1 << 11 };


// Cloud tests: MIRROR_PADTEST=FILE plays a virtual pad. Lines "FRAME BUTTONS [RX RY RT]":
// BUTTONS like b+dpleft or - (SDL's names), RX RY RT the right stick and trigger, -1..1.
struct PadStep { uint32_t frame; uint32_t buttons; float rx, ry, rt; };
static std::vector<PadStep> testSteps;
static size_t testAt = 0;
static uint32_t testFrame = 0;
static SDL_Joystick* testPad = nullptr;

static void LoadPadTest(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); return; }
    char line[256];
    while (fgets(line, sizeof line, f))
    {
        if (line[0] == '#') continue;
        PadStep st {};
        char keys[128];
        int n = sscanf(line, "%u %127s %f %f %f", &st.frame, keys, &st.rx, &st.ry, &st.rt);
        if (n < 2) continue;
        if (strcmp(keys, "-"))
            for (char* k = strtok(keys, "+"); k; k = strtok(nullptr, "+"))
            {
                SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(k);
                if (b != SDL_CONTROLLER_BUTTON_INVALID) st.buttons |= 1u << b;
                else fprintf(stderr, "pads: test: no button %s\n", k);
            }
        testSteps.push_back(st);
    }
    fclose(f);
    int i = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    testPad = i >= 0 ? SDL_JoystickOpen(i) : nullptr;
    fprintf(stderr, "pads: test pad with %zu steps%s\n", testSteps.size(), testPad ? "" : " (no virtual pad)");
}

static void PlayPadTest()
{
    if (!testPad) return;
    while (testAt < testSteps.size() && testSteps[testAt].frame <= testFrame)
    {
        const PadStep& st = testSteps[testAt++];
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
            SDL_JoystickSetVirtualButton(testPad, b, (st.buttons >> b) & 1);
        SDL_JoystickSetVirtualAxis(testPad, SDL_CONTROLLER_AXIS_RIGHTX, (Sint16)(st.rx * 32767));
        SDL_JoystickSetVirtualAxis(testPad, SDL_CONTROLLER_AXIS_RIGHTY, (Sint16)(st.ry * 32767));
        SDL_JoystickSetVirtualAxis(testPad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, (Sint16)(st.rt * 32767));
    }
    testFrame++;
}

bool SionPads::Init()
{
    // mirrorrun stops cleanly on SIGINT/SIGTERM itself; no window, so events come regardless
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0)
    {
        fprintf(stderr, "pads: SDL: %s\n", SDL_GetError());
        return false;
    }
    const char* db = getenv("MIRROR_PADDB");
    if (!db) db = "/opt/xbcompat/share/gamecontrollerdb.txt";
    if (access(db, R_OK) == 0)
    {
        int n = SDL_GameControllerAddMappingsFromFile(db);
        if (n > 0) fprintf(stderr, "pads: %d mappings from %s\n", n, db);
    }
    return true;
}

bool SionPads::Open()
{
    if (const char* t = getenv("MIRROR_PADTEST"))
    {
        // cloud tests: the virtual pad steps a frame at a time, so SDL stays on the game's thread
        if (!Init()) return false;
        open = true;
        LoadPadTest(t);
        return true;
    }
    std::promise<bool> ready;
    auto started = ready.get_future();
    thread = std::thread([this, &ready] {
        pthread_setname_np(pthread_self(), "pads");
        bool ok = Init();
        ready.set_value(ok);
        if (!ok) return;
        while (!stop)
        {
            State st;
            Read(st);
            {
                std::lock_guard<std::mutex> g(lock);
                latest = st;
                pressed.held |= st.held;
                pressed.menu |= st.menu;
                pressed.back |= st.back;
                pressed.touch |= st.touch;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        for (void* p : pads) SDL_GameControllerClose((SDL_GameController*)p);
        pads.clear();
        SDL_Quit();
    });
    open = threaded = started.get();
    if (!open) thread.join();
    return open;
}

void SionPads::Close()
{
    if (!open) return;
    if (threaded)
    {
        stop = true;
        thread.join();
    }
    else
    {
        for (void* p : pads) SDL_GameControllerClose((SDL_GameController*)p);
        pads.clear();
        if (testPad) SDL_JoystickClose(testPad);
        testPad = nullptr;
        SDL_Quit();
    }
    open = threaded = false;
}

void SionPads::Read(State& st)
{
    SDL_GameControllerUpdate();
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        if (e.type == SDL_CONTROLLERDEVICEADDED)
        {
            if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which))
            {
                pads.push_back(c);
                fprintf(stderr, "pads: %s\n", SDL_GameControllerName(c));
            }
        }
        else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
        {
            for (size_t i = 0; i < pads.size(); i++)
            {
                SDL_GameController* c = (SDL_GameController*)pads[i];
                if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == e.cdevice.which)
                {
                    fprintf(stderr, "pads: %s went away\n", SDL_GameControllerName(c));
                    SDL_GameControllerClose(c);
                    pads.erase(pads.begin() + i);
                    break;
                }
            }
        }
    }

    // every pad plays: buttons held on any of them count
    for (void* p : pads)
    {
        SDL_GameController* c = (SDL_GameController*)p;
        auto b = [c](SDL_GameControllerButton k) { return SDL_GameControllerGetButton(c, k) != 0; };
        auto ax = [c](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(c, a) / 32767.0f; };
        if (b(SDL_CONTROLLER_BUTTON_B)) st.held |= DS_A;
        if (b(SDL_CONTROLLER_BUTTON_A)) st.held |= DS_B;
        if (b(SDL_CONTROLLER_BUTTON_Y)) st.held |= DS_X;
        if (b(SDL_CONTROLLER_BUTTON_X)) st.held |= DS_Y;
        if (b(SDL_CONTROLLER_BUTTON_START)) st.held |= DS_START;
        if (b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) st.held |= DS_L;
        if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) st.held |= DS_R;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_UP)) st.held |= DS_UP;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) st.held |= DS_DOWN;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) st.held |= DS_LEFT;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) st.held |= DS_RIGHT;
        st.back |= b(SDL_CONTROLLER_BUTTON_BACK);
        float lx = ax(SDL_CONTROLLER_AXIS_LEFTX), ly = ax(SDL_CONTROLLER_AXIS_LEFTY);
        if (lx * lx + ly * ly > 0.25f)
        {
            // eight ways: a direction counts within 67.5 degrees of it
            float a = atan2f(ly, lx);
            if (cosf(a) > 0.38f) st.held |= DS_RIGHT;
            if (cosf(a) < -0.38f) st.held |= DS_LEFT;
            if (sinf(a) > 0.38f) st.held |= DS_DOWN;
            if (sinf(a) < -0.38f) st.held |= DS_UP;
        }
        // the menu goes by the labels, as the dashboard does: A picks, B goes back
        if (b(SDL_CONTROLLER_BUTTON_A)) st.menu |= MK_OK;
        if (b(SDL_CONTROLLER_BUTTON_B)) st.menu |= MK_BACK;
        if (b(SDL_CONTROLLER_BUTTON_BACK) || b(SDL_CONTROLLER_BUTTON_START)) st.menu |= MK_CLOSE;
        float rx = ax(SDL_CONTROLLER_AXIS_RIGHTX), ry = ax(SDL_CONTROLLER_AXIS_RIGHTY);
        if (fabsf(rx) > fabsf(st.sx)) st.sx = rx;
        if (fabsf(ry) > fabsf(st.sy)) st.sy = ry;
        if (ax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f || b(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) st.touch = true;
    }
    if (st.held & DS_UP) st.menu |= MK_UP;
    if (st.held & DS_DOWN) st.menu |= MK_DOWN;
    if (st.held & DS_LEFT) st.menu |= MK_LEFT;
    if (st.held & DS_RIGHT) st.menu |= MK_RIGHT;
}

uint16_t SionPads::MenuKeys()
{
    if (!open) return 0;
    if (!threaded)
    {
        PlayPadTest();
        State st;
        Read(st);
        return st.menu;
    }
    std::lock_guard<std::mutex> g(lock);
    uint16_t k = latest.menu | pressed.menu;
    pressed.menu = 0;
    return k;
}

void SionPads::Swallow()
{
    if (!open) return;
    State st;
    if (threaded)
    {
        std::lock_guard<std::mutex> g(lock);
        st = latest;
        pressed = {};
    }
    else
        Read(st);
    swallowed = st.held;
    backSwallowed = st.back;
}

void SionPads::Poll(uint16_t& held, bool& touch, uint16_t& x, uint16_t& y)
{
    held = 0;
    touch = false;
    if (!open) return;
    State st;
    if (threaded)
    {
        // what's held now, and what was pressed and let go since the last frame
        std::lock_guard<std::mutex> g(lock);
        st = latest;
        st.held |= pressed.held;
        st.back |= pressed.back;
        st.touch |= pressed.touch;
        pressed = {};
    }
    else
    {
        PlayPadTest();
        Read(st);
    }
    // what closed the menu stays out of the game until it's let go
    swallowed &= st.held;
    held = st.held & ~swallowed;
    touch = st.touch;
    if (backSwallowed && !st.back) backSwallowed = false;
    bool back = st.back && !backSwallowed;
    float sx = st.sx, sy = st.sy;

    // Back: Select for the game, until it's been held 1.5 s; then the menu, and the game's
    // Select is let go until Back is
    if (back)
    {
        if (++backFrames == MenuHoldFrames)
        {
            menuAsked = true;
            backSpent = true;
        }
        if (!backSpent) held |= DS_SELECT;
    }
    else
    {
        backFrames = 0;
        backSpent = false;
    }

    // the stylus: slow near the middle of the stick for aiming, up to 6 px a frame
    float m = sqrtf(sx * sx + sy * sy);
    if (m > 0.15f)
    {
        float speed = 6.0f * powf((m - 0.15f) / 0.85f, 1.8f);
        cx = fminf(255.0f, fmaxf(0.0f, cx + sx / m * speed));
        cy = fminf(191.0f, fmaxf(0.0f, cy + sy / m * speed));
        cursorFrames = 180;
    }
    else if (touch)
        cursorFrames = 180;
    else if (cursorFrames > 0)
        cursorFrames--;
    x = (uint16_t)cx;
    y = (uint16_t)cy;
}

#else

bool SionPads::Open()
{
    fprintf(stderr, "pads: this build has no SDL\n");
    return false;
}
void SionPads::Close() {}
uint16_t SionPads::MenuKeys() { return 0; }
void SionPads::Swallow() {}
void SionPads::Poll(uint16_t& held, bool& touch, uint16_t&, uint16_t&)
{
    held = 0;
    touch = false;
}

#endif

void DrawCursor(uint32_t* s, int x, int y, bool touching)
{
    // a small cross with a dark edge, filled in while it touches
    auto put = [s](int px, int py, uint32_t c) {
        if (px >= 0 && px < 256 && py >= 0 && py < 192) s[py * 256 + px] = c;
    };
    const uint32_t light = touching ? 0xFFF0C040 : 0xFFFFFFFF, dark = 0xFF202020;
    for (int d = -5; d <= 5; d++)
    {
        put(x + d, y - 1, dark); put(x + d, y + 1, dark);
        put(x - 1, y + d, dark); put(x + 1, y + d, dark);
    }
    for (int d = -4; d <= 4; d++)
    {
        put(x + d, y, light);
        put(x, y + d, light);
    }
}
