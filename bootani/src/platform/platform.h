//
//  platform.h
//
//  Everything host-specific the port needs: an OpenGL 3.3 core context
//  (in a window or headless), a millisecond clock, and an audio output.
//
#ifndef BOOTANI_PLATFORM_H
#define BOOTANI_PLATFORM_H

#include <stdint.h>

struct PlatformConfig
{
    int  width;         // window size (the animation renders at 640x480)
    int  height;
    bool headless;      // no window: render off-screen
    bool fullscreen;
    bool vsync;
    bool kms;                 // Linux console: draw straight to the display (DRM/KMS)
    const char* drm_device;   // KMS: /dev/dri/cardN, or NULL for the first with a display
    const char* audio_device; // ALSA PCM name, or NULL for the default
};

// Create the GL context. Returns false with a message on stderr on failure.
bool  Platform_Init(const PlatformConfig& cfg);
void  Platform_Shutdown();
void* Platform_GetProcAddress(const char* name);
const char* Platform_Describe();          // e.g. "SDL2 window" / "EGL (headless)" / "KMS 1920x1080@60"

// Show a finished frame (window or KMS; nothing when headless). fbo holds the image with row 0 at
// the top; it is scaled to the window with the aspect ratio preserved.
void  Platform_ShowFrame(unsigned int fbo, int width, int height);

// Handle window events. Returns false once the user asked to quit.
bool  Platform_PumpEvents();

uint64_t Platform_TicksMs();

// Audio: 48 kHz interleaved stereo int16, pulled from a callback on the
// audio thread. Lock/Unlock serialise against that callback.
typedef void (*PlatformAudioCallback)(int16_t* out, int frames, void* user);
bool  Platform_AudioOpen(int rate, PlatformAudioCallback cb, void* user);
void  Platform_AudioLock();
void  Platform_AudioUnlock();
void  Platform_AudioClose();

#endif
