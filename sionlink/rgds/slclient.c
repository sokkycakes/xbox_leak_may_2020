// Sion Link client for Linux handhelds: the native counterpart of the DS
// Sion Link app (ds/source/main.c). Finds the Sion, links like a DS does
// (sllinux.h), shows its welcome and texts, pings it with A, unlinks with B.
//
// The Sion's Download Play sends DS programs, which this can't run: it turns
// them down (SL_FILE_REFUSED) and says what was offered. Native clients of
// Sion games will link with their own app ID instead.
//
//   slclient [--headless] [--font FILE.ttf] [--name NAME] [--iface IF]
//            [--direct-peer MAC]   (testing without a radio, see sllinux.h)
//   slclient --join-only [--iface IF]
//            joins the Sion's DS network and stays on it (no Sion Link hello) until
//            stopped, printing "joined BSSID" once there: the DS mirror's way onto it
//            when the handheld and the Sion aren't on the same home Wi-Fi
//
// SDL2 + SDL2_ttf from the system; draws on the first display (BananaOS's
// ports: the top panel).

#ifndef SL_NO_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#endif
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sllinux.h"

static bool headless, join_only;
#ifndef SL_NO_SDL
static SDL_Window *win;
static SDL_Renderer *ren;
static TTF_Font *font, *big;
#endif
static volatile sig_atomic_t stop;
static bool quit_pressed, a_pressed;
static char status_text[256];

#define W 640
#define H 480

static void Delay(int ms)
{
    struct timespec t = { 0, ms * 1000000L };
    nanosleep(&t, NULL);
}

#ifdef SL_NO_SDL
// Headless build (tests, no SDL): only --headless works
static void Input(void) {}
static void Screen(const char *title, const char *body, const char *lines[], int nlines, const char *foot) {}
#else

static const SDL_Color ink = { 0xF1, 0xED, 0xE2, 0xFF }, dim = { 0x9A, 0x96, 0x8C, 0xFF },
                       accent = { 0xF0, 0xB4, 0x29, 0xFF }, bg = { 0x17, 0x16, 0x1C, 0xFF };

static void Text(TTF_Font *f, int x, int y, SDL_Color c, const char *s)
{
    if (!s[0])
        return;
    SDL_Surface *su = TTF_RenderUTF8_Blended_Wrapped(f, s, c, W - x - 24);
    if (!su)
        return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, su);
    SDL_Rect r = { x, y, su->w, su->h };
    SDL_RenderCopy(ren, t, NULL, &r);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(su);
}

static void Input(void)
{
    if (headless)
        return;
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        if (e.type == SDL_QUIT)
            quit_pressed = true;
        else if (e.type == SDL_CONTROLLERDEVICEADDED)
            SDL_GameControllerOpen(e.cdevice.which);
        else if (e.type == SDL_CONTROLLERBUTTONDOWN)
        {
            // SDL's names are by position: on Nintendo-style pads "B" (south)
            // is the A label, so take the face buttons by the labels players see
            int b = e.cbutton.button;
            if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_START)
                a_pressed = true;
            else if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_BACK)
                quit_pressed = true;
        }
        else if (e.type == SDL_KEYDOWN)
        {
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_RETURN || k == SDLK_a || k == SDLK_SPACE)
                a_pressed = true;
            else if (k == SDLK_ESCAPE || k == SDLK_b || k == SDLK_BACKSPACE)
                quit_pressed = true;
        }
    }
}

static void Screen(const char *title, const char *body, const char *lines[], int nlines, const char *foot)
{
    if (headless)
        return;
    SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawColor(ren, accent.r, accent.g, accent.b, 255);
    SDL_Rect bar = { 0, 0, W, 6 };
    SDL_RenderFillRect(ren, &bar);
    Text(big, 24, 24, accent, title);
    Text(font, 24, 84, ink, body);
    for (int i = 0; i < nlines; i++)
        Text(font, 24, 260 + i * 30, ink, lines[i]);
    Text(font, 24, H - 44, dim, foot);
    const char *shot = getenv("SL_SCREENSHOT"); // checking the layout
    if (shot)
    {
        int ow = W, oh = H;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_Surface *su = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
        if (su && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, su->pixels, su->pitch) == 0)
            SDL_SaveBMP(su, shot);
        SDL_FreeSurface(su);
    }
    SDL_RenderPresent(ren);
}
#endif

static void JoinStatus(const char *s)
{
    if (strcmp(s, status_text))
    {
        snprintf(status_text, sizeof(status_text), "%s", s);
        printf("slclient: %s\n", s);
        fflush(stdout);
    }
    Screen("Sion Link", status_text, NULL, 0, "B: quit");
}

static bool JoinCancel(void)
{
    Input();
    JoinStatus(status_text);
    return stop || quit_pressed;
}

static void OnSignal(int s) { stop = 1; (void)s; }

static void Log(const char *fmt, const char *a, const char *b)
{
    printf("slclient: ");
    printf(fmt, a, b);
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *font_path = NULL;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--headless"))
            headless = true;
        else if (!strcmp(argv[i], "--join-only"))
            headless = join_only = true;
        else if (!strcmp(argv[i], "--font") && i + 1 < argc)
            font_path = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            SlSetName(argv[++i]);
        else if (!strcmp(argv[i], "--iface") && i + 1 < argc)
            SlSetInterface(argv[++i]);
        else if (!strcmp(argv[i], "--direct-peer") && i + 1 < argc)
        {
            unsigned m[6];
            uint8_t mac[6];
            if (sscanf(argv[++i], "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
                return 2;
            for (int j = 0; j < 6; j++)
                mac[j] = m[j];
            SlSetDirectPeer(mac);
        }
        else
        {
            fprintf(stderr, "usage: slclient [--headless | --join-only] [--font FILE.ttf] [--name NAME] [--iface IF] [--direct-peer MAC]\n");
            return 2;
        }
    }
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    signal(SIGHUP, OnSignal);

#ifdef SL_NO_SDL
    headless = true;
    (void)font_path;
#else
    if (!headless)
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0 || TTF_Init() < 0)
        {
            fprintf(stderr, "slclient: SDL: %s\n", SDL_GetError());
            return 1;
        }
        win = SDL_CreateWindow("Sion Link", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H,
                               SDL_WINDOW_FULLSCREEN_DESKTOP);
        ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (!ren)
        {
            fprintf(stderr, "slclient: window: %s\n", SDL_GetError());
            return 1;
        }
        SDL_RenderSetLogicalSize(ren, W, H);
        if (!font_path)
            font_path = "font.ttf";
        font = TTF_OpenFont(font_path, 22);
        big = TTF_OpenFont(font_path, 36);
        if (!font || !big)
        {
            fprintf(stderr, "slclient: font %s: %s\n", font_path, TTF_GetError());
            return 1;
        }
        SDL_ShowCursor(SDL_DISABLE);
    }
#endif

    int rc = 0;
    if (!SlInit())
    {
        snprintf(status_text, sizeof(status_text), "Couldn't open the Wi-Fi interface.\nNeeds root, and Wi-Fi switched on.");
        printf("slclient: %s\n", status_text);
        for (uint32_t t = SlMillis(); SlMillis() - t < 4000 && !quit_pressed && !stop;)
        {
            Input();
            Screen("Sion Link", status_text, NULL, 0, "B: quit");
            Delay(16);
        }
        rc = 1;
        goto out;
    }

    SlSetCancel(JoinCancel);
    if (join_only)
    {
        if (SlJoin(JoinStatus))
        {
            uint8_t b[6];
            char ssid[33];
            int freq;
            SlNetwork(b, ssid, &freq);
            printf("joined %02x:%02x:%02x:%02x:%02x:%02x\n", b[0], b[1], b[2], b[3], b[4], b[5]);
            fflush(stdout);
            while (!stop)
                Delay(200);
        }
        else
            rc = 1;
        goto out;
    }
    while (!stop && !quit_pressed)
    {
        if (!SlJoin(JoinStatus))
            break;
        uint8_t bssid[6];
        char ssid[33];
        int freq;
        SlNetwork(bssid, ssid, &freq);
        printf("slclient: joined \"%s\" %02x:%02x:%02x:%02x:%02x:%02x %d MHz\n", ssid, bssid[0], bssid[1],
               bssid[2], bssid[3], bssid[4], bssid[5], freq);
        fflush(stdout);

        sl_welcome welcome;
        memset(&welcome, 0, sizeof(welcome));
        bool welcomed = false, lost = false;
        uint32_t pings = 0, pongs = 0, rtt = 0, last_auto = 0;
        char text[4][33] = { { 0 } };
        char note[80] = "";
        uint32_t t0 = SlMillis();

        while (!stop && !quit_pressed)
        {
            Input();
            uint32_t now = SlMillis();
            SlKeepAlive(0, now);

            // A pings; headless runs ping once a second
            if (welcomed && (a_pressed || (headless && now - last_auto >= 1000)))
            {
                sl_ping p = { now, ++pings };
                SlSend(SL_PING, &p, sizeof(p));
                last_auto = now;
            }
            a_pressed = false;

            const sl_hdr *h;
            const uint8_t *body;
            while (SlNext(&h, &body))
            {
                if (h->type == SL_WELCOME && h->len >= sizeof(sl_welcome))
                {
                    if (!welcomed)
                    {
                        memcpy(&welcome, body, sizeof(welcome));
                        welcome.host[sizeof(welcome.host) - 1] = 0;
                        welcome.motd[sizeof(welcome.motd) - 1] = 0;
                        Log("welcomed by %s: %s", welcome.host, welcome.motd);
                    }
                    memcpy(&welcome, body, sizeof(welcome));
                    welcome.host[sizeof(welcome.host) - 1] = 0;
                    welcome.motd[sizeof(welcome.motd) - 1] = 0;
                    welcomed = true;
                }
                else if (h->type == SL_PONG && h->len >= sizeof(sl_ping))
                {
                    sl_ping p;
                    memcpy(&p, body, sizeof(p));
                    rtt = now - p.stamp;
                    pongs++;
                    if (headless)
                        printf("slclient: pong %u, %u ms\n", p.count, rtt), fflush(stdout);
                }
                else if (h->type == SL_TEXT)
                {
                    memmove(text[0], text[1], sizeof(text[0]) * 3);
                    int n = h->len < 32 ? h->len : 32;
                    memcpy(text[3], body, n);
                    text[3][n] = 0;
                    Log("text: %s%s", text[3], "");
                }
                else if (h->type == SL_FILE_BEGIN && h->len >= sizeof(sl_file_begin))
                {
                    sl_file_begin b;
                    memcpy(&b, body, sizeof(b));
                    b.name[sizeof(b.name) - 1] = 0;
                    sl_file_ack a = { .id = b.id, .next = 0, .status = SL_FILE_REFUSED };
                    SlSend(SL_FILE_ACK, &a, sizeof(a));
                    snprintf(note, sizeof(note), "Turned down %s (a DS program)", b.name);
                    Log("%s%s", note, "");
                }
                else if (h->type == SL_BYE)
                {
                    snprintf(note, sizeof(note), "The Sion ended the link.");
                    lost = true;
                }
            }

            if (!welcomed && now - t0 > 30000)
            {
                snprintf(note, sizeof(note), "The Sion didn't answer.");
                lost = true;
            }
            if (welcomed && SlQuietFor(now) > 5000)
            {
                snprintf(note, sizeof(note), "Lost the Sion.");
                lost = true;
            }
            if (lost)
            {
                Log("%s%s", note, "");
                break;
            }

            char body_text[256], stats[96], who[64];
            if (welcomed)
            {
                snprintf(body_text, sizeof(body_text), "Linked to %s, player #%d of %d\n\n%s", welcome.host,
                         welcome.client_id, welcome.clients, welcome.motd);
                snprintf(stats, sizeof(stats), "Pings %u, answered %u, last %u ms", pings, pongs, rtt);
            }
            else
            {
                snprintf(body_text, sizeof(body_text), "Saying hello to the Sion...");
                stats[0] = 0;
            }
            snprintf(who, sizeof(who), "%s", note);
            const char *lines[] = { stats, text[0], text[1], text[2], text[3], who };
            Screen("Sion Link", body_text, lines, 6, "A: ping    B: unlink");
            Delay(headless ? 5 : 1);
        }
        if (stop || quit_pressed)
            break;
        // Lost it: show why for a moment, then look again
        for (uint32_t t = SlMillis(); SlMillis() - t < 2000 && !quit_pressed && !stop;)
        {
            Input();
            Screen("Sion Link", note, NULL, 0, "Looking again... B: quit");
            Delay(16);
        }
    }
out:
    SlExit();
#ifndef SL_NO_SDL
    if (!headless)
    {
        TTF_Quit();
        SDL_Quit();
    }
#endif
    return rc;
}
