//
//  backends.h
//
//  Internal interface between platform.cpp and the context backends.
//
#ifndef BOOTANI_PLATFORM_BACKENDS_H
#define BOOTANI_PLATFORM_BACKENDS_H

#include "platform.h"

#ifdef BOOTANI_HAVE_SDL2
bool  Sdl_Init(const PlatformConfig& cfg);
void  Sdl_Shutdown();
void* Sdl_GetProcAddress(const char* name);
void  Sdl_ShowFrame(unsigned int fbo, int width, int height);
bool  Sdl_PumpEvents();
bool  Sdl_AudioOpen(int rate, PlatformAudioCallback cb, void* user);
void  Sdl_AudioLock();
void  Sdl_AudioUnlock();
void  Sdl_AudioClose();
#endif

#ifdef BOOTANI_HAVE_EGL
bool  Egl_Init(const PlatformConfig& cfg);
void  Egl_Shutdown();
void* Egl_GetProcAddress(const char* name);
#endif

#ifdef BOOTANI_HAVE_KMS
bool  Kms_Init(const PlatformConfig& cfg);
void  Kms_Shutdown();
void* Kms_GetProcAddress(const char* name);
const char* Kms_Describe();
void  Kms_ShowFrame(unsigned int fbo, int width, int height);
#endif

#ifdef BOOTANI_HAVE_ALSA
bool  Alsa_AudioOpen(const char* device, int rate, PlatformAudioCallback cb, void* user);
void  Alsa_AudioLock();
void  Alsa_AudioUnlock();
void  Alsa_AudioClose();
#endif

#endif
