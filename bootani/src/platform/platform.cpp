//
//  platform.cpp
//
//  Picks a context backend: SDL2 for windows (and as the headless fallback
//  on hosts without EGL), EGL surfaceless for headless Linux, KMS for a
//  Linux console with no window system. Audio goes through SDL2 when the
//  window is SDL's, else straight to ALSA.
//
#include "backends.h"
#include <chrono>
#include <stdio.h>

enum Backend { BACKEND_NONE, BACKEND_SDL, BACKEND_EGL, BACKEND_KMS };
static Backend g_backend = BACKEND_NONE;
static bool    g_headless = false;
static const char* g_audio_device = NULL;

enum AudioBackend { AUDIO_NONE, AUDIO_SDL, AUDIO_ALSA };
static AudioBackend g_audio = AUDIO_NONE;

bool Platform_Init(const PlatformConfig& cfg)
{
    g_headless = cfg.headless;
    g_audio_device = cfg.audio_device;
#ifdef BOOTANI_HAVE_EGL
    if (cfg.headless) {
        if (Egl_Init(cfg)) { g_backend = BACKEND_EGL; return true; }
        fprintf(stderr, "bootani: EGL headless context unavailable, trying SDL\n");
    }
#endif
#ifdef BOOTANI_HAVE_KMS
    if (cfg.kms && !cfg.headless) {
        if (Kms_Init(cfg)) { g_backend = BACKEND_KMS; return true; }
        return false;
    }
#endif
#ifdef BOOTANI_HAVE_SDL2
    if (Sdl_Init(cfg)) { g_backend = BACKEND_SDL; return true; }
#endif
#ifdef BOOTANI_HAVE_KMS
    // No SDL2 (or no window system for it): try the console directly.
    if (!cfg.headless && Kms_Init(cfg)) { g_backend = BACKEND_KMS; return true; }
#endif
    fprintf(stderr, "bootani: no usable OpenGL 3.3 context (built with%s%s%s)\n",
#ifdef BOOTANI_HAVE_SDL2
            " SDL2",
#else
            "",
#endif
#ifdef BOOTANI_HAVE_EGL
            " EGL",
#else
            "",
#endif
#ifdef BOOTANI_HAVE_KMS
            " KMS"
#else
            ""
#endif
            );
    return false;
}

void Platform_Shutdown()
{
#ifdef BOOTANI_HAVE_EGL
    if (g_backend == BACKEND_EGL) Egl_Shutdown();
#endif
#ifdef BOOTANI_HAVE_KMS
    if (g_backend == BACKEND_KMS) Kms_Shutdown();
#endif
#ifdef BOOTANI_HAVE_SDL2
    if (g_backend == BACKEND_SDL) Sdl_Shutdown();
#endif
    g_backend = BACKEND_NONE;
}

void* Platform_GetProcAddress(const char* name)
{
#ifdef BOOTANI_HAVE_EGL
    if (g_backend == BACKEND_EGL) return Egl_GetProcAddress(name);
#endif
#ifdef BOOTANI_HAVE_KMS
    if (g_backend == BACKEND_KMS) return Kms_GetProcAddress(name);
#endif
#ifdef BOOTANI_HAVE_SDL2
    if (g_backend == BACKEND_SDL) return Sdl_GetProcAddress(name);
#endif
    return 0;
}

const char* Platform_Describe()
{
    switch (g_backend) {
    case BACKEND_EGL: return "EGL (headless)";
    case BACKEND_SDL: return g_headless ? "SDL2 (hidden window)" : "SDL2 window";
#ifdef BOOTANI_HAVE_KMS
    case BACKEND_KMS: return Kms_Describe();
#endif
    default:          return "none";
    }
}

void Platform_ShowFrame(unsigned int fbo, int width, int height)
{
#ifdef BOOTANI_HAVE_SDL2
    if (g_backend == BACKEND_SDL && !g_headless) Sdl_ShowFrame(fbo, width, height);
#endif
#ifdef BOOTANI_HAVE_KMS
    if (g_backend == BACKEND_KMS) Kms_ShowFrame(fbo, width, height);
#endif
    (void)fbo; (void)width; (void)height;
}

bool Platform_PumpEvents()
{
#ifdef BOOTANI_HAVE_SDL2
    if (g_backend == BACKEND_SDL) return Sdl_PumpEvents();
#endif
    return true;
}

uint64_t Platform_TicksMs()
{
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now() - start).count();
}

bool Platform_AudioOpen(int rate, PlatformAudioCallback cb, void* user)
{
#ifdef BOOTANI_HAVE_SDL2
    if (g_backend == BACKEND_SDL) {
        if (Sdl_AudioOpen(rate, cb, user)) { g_audio = AUDIO_SDL; return true; }
    }
#endif
#ifdef BOOTANI_HAVE_ALSA
    if (Alsa_AudioOpen(g_audio_device, rate, cb, user)) { g_audio = AUDIO_ALSA; return true; }
#endif
#ifdef BOOTANI_HAVE_SDL2
    if (g_backend != BACKEND_SDL && Sdl_AudioOpen(rate, cb, user)) { g_audio = AUDIO_SDL; return true; }
#endif
    (void)rate; (void)cb; (void)user;
    return false;
}

void Platform_AudioLock()
{
#ifdef BOOTANI_HAVE_SDL2
    if (g_audio == AUDIO_SDL) Sdl_AudioLock();
#endif
#ifdef BOOTANI_HAVE_ALSA
    if (g_audio == AUDIO_ALSA) Alsa_AudioLock();
#endif
}

void Platform_AudioUnlock()
{
#ifdef BOOTANI_HAVE_SDL2
    if (g_audio == AUDIO_SDL) Sdl_AudioUnlock();
#endif
#ifdef BOOTANI_HAVE_ALSA
    if (g_audio == AUDIO_ALSA) Alsa_AudioUnlock();
#endif
}

void Platform_AudioClose()
{
#ifdef BOOTANI_HAVE_SDL2
    if (g_audio == AUDIO_SDL) Sdl_AudioClose();
#endif
#ifdef BOOTANI_HAVE_ALSA
    if (g_audio == AUDIO_ALSA) Alsa_AudioClose();
#endif
    g_audio = AUDIO_NONE;
}
