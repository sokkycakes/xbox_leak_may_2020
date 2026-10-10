//
//  platform_sdl.cpp
//
//  Window, GL context, input and audio through SDL2 (Linux, Windows, macOS).
//
#include "backends.h"
#include "../gfx/gl_loader.h"
// main() lives in main.cpp and stays a plain C main; no SDL2main.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>

static SDL_Window*       g_window = NULL;
static SDL_GLContext     g_gl = NULL;
static SDL_AudioDeviceID g_audio = 0;
static PlatformAudioCallback g_audio_cb = NULL;
static void*             g_audio_user = NULL;

bool Sdl_Init(const PlatformConfig& cfg)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "bootani: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.headless) flags |= SDL_WINDOW_HIDDEN;
    else flags |= SDL_WINDOW_RESIZABLE;
    if (cfg.fullscreen && !cfg.headless) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_window = SDL_CreateWindow("Xbox boot animation", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                cfg.width, cfg.height, flags);
    if (!g_window) {
        fprintf(stderr, "bootani: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) {
        fprintf(stderr, "bootani: OpenGL 3.3 core context failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        SDL_Quit();
        return false;
    }
    SDL_GL_SetSwapInterval(cfg.vsync ? 1 : 0);
    if (cfg.fullscreen) SDL_ShowCursor(SDL_DISABLE);
    return true;
}

void Sdl_Shutdown()
{
    Sdl_AudioClose();
    if (g_gl) SDL_GL_DeleteContext(g_gl);
    if (g_window) SDL_DestroyWindow(g_window);
    g_gl = NULL;
    g_window = NULL;
    SDL_Quit();
}

void* Sdl_GetProcAddress(const char* name)
{
    return SDL_GL_GetProcAddress(name);
}

void Sdl_ShowFrame(unsigned int fbo, int width, int height)
{
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    // Letterbox to 4:3 (the animation's aspect).
    int w = dw, h = dw * height / width;
    if (h > dh) { h = dh; w = dh * width / height; }
    int x = (dw - w) / 2, y = (dh - h) / 2;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, dw, dh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    // Source row 0 is the top of the image; the window's row 0 is the bottom.
    glBlitFramebuffer(0, height, width, 0, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    SDL_GL_SwapWindow(g_window);
}

bool Sdl_PumpEvents()
{
    SDL_Event e;
    bool keep_going = true;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) keep_going = false;
        if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q))
            keep_going = false;
    }
    return keep_going;
}

static void SDLCALL AudioThunk(void*, Uint8* stream, int len)
{
    int frames = len / 4;
    if (g_audio_cb) g_audio_cb((int16_t*)stream, frames, g_audio_user);
    else SDL_memset(stream, 0, len);
}

bool Sdl_AudioOpen(int rate, PlatformAudioCallback cb, void* user)
{
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "bootani: no audio: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = AudioThunk;
    g_audio_cb = cb;
    g_audio_user = user;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_audio) {
        fprintf(stderr, "bootani: no audio device: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(g_audio, 0);
    return true;
}

void Sdl_AudioLock()   { if (g_audio) SDL_LockAudioDevice(g_audio); }
void Sdl_AudioUnlock() { if (g_audio) SDL_UnlockAudioDevice(g_audio); }

void Sdl_AudioClose()
{
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    g_audio = 0;
}
