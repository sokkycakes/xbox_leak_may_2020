//
//  platform_egl.cpp
//
//  Headless OpenGL 3.3 core context through EGL, with no window system:
//  EGL_MESA_platform_surfaceless when available (works with Mesa's llvmpipe
//  on machines without a GPU), otherwise the default display. The animation
//  renders only into framebuffer objects, so no EGL surface is needed.
//
#include "backends.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <string.h>

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;

static EGLDisplay OpenDisplay()
{
    const char* client_ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (client_ext && strstr(client_ext, "EGL_MESA_platform_surfaceless")) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_platform_display) {
            EGLDisplay d = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
            if (d != EGL_NO_DISPLAY) return d;
        }
    }
    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

bool Egl_Init(const PlatformConfig&)
{
    g_display = OpenDisplay();
    if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, NULL, NULL)) {
        fprintf(stderr, "bootani: eglInitialize failed\n");
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "bootani: EGL has no desktop OpenGL\n");
        return false;
    }
    const EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, 0,
        EGL_NONE
    };
    EGLConfig config = 0;
    EGLint count = 0;
    EGLConfig* configp = &config;
    if (!eglChooseConfig(g_display, config_attribs, configp, 1, &count) || count == 0)
        configp = NULL;   // EGL_KHR_no_config_context
    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    g_context = eglCreateContext(g_display, configp ? config : (EGLConfig)0, EGL_NO_CONTEXT, context_attribs);
    if (g_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "bootani: eglCreateContext (GL 3.3 core) failed: 0x%x\n", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, g_context)) {
        fprintf(stderr, "bootani: eglMakeCurrent without a surface failed: 0x%x\n", eglGetError());
        return false;
    }
    return true;
}

void Egl_Shutdown()
{
    if (g_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
        eglTerminate(g_display);
    }
    g_display = EGL_NO_DISPLAY;
    g_context = EGL_NO_CONTEXT;
}

void* Egl_GetProcAddress(const char* name)
{
    return (void*)eglGetProcAddress(name);
}
