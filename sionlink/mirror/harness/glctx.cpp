#include "glctx.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <vector>

#include "frontend/glad/glad.h"

// the few EGL calls needed, looked up in libEGL at run time
typedef void* EGLDisplay;
typedef void* EGLContext;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef int32_t EGLint;
typedef unsigned EGLenum;
typedef unsigned EGLBoolean;
static const EGLint EGL_NONE_ = 0x3038, EGL_CONTEXT_MAJOR_VERSION_ = 0x3098, EGL_CONTEXT_MINOR_VERSION_ = 0x30FB,
                    EGL_CONTEXT_OPENGL_PROFILE_MASK_ = 0x30FD, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_ = 1,
                    EGL_VENDOR_ = 0x3053, EGL_PLATFORM_SURFACELESS_MESA_ = 0x31DD;
static const EGLenum EGL_OPENGL_API_ = 0x30A2;

static void* egl;
static EGLDisplay dpy;
static EGLContext ctx;
static void* (*getProc)(const char*);
static EGLBoolean (*eglMakeCurrent_)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*eglDestroyContext_)(EGLDisplay, EGLContext);
static EGLBoolean (*eglTerminate_)(EGLDisplay);
static GLuint readFB;

bool GLContextOpen()
{
    egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!egl) { fprintf(stderr, "gl: no libEGL.so.1 (%s)\n", dlerror()); return false; }
    getProc = (void* (*)(const char*))dlsym(egl, "eglGetProcAddress");
    auto getPlatformDisplay = (EGLDisplay (*)(EGLenum, void*, const intptr_t*))dlsym(egl, "eglGetPlatformDisplay");
    auto initialize = (EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*))dlsym(egl, "eglInitialize");
    auto bindAPI = (EGLBoolean (*)(EGLenum))dlsym(egl, "eglBindAPI");
    auto createContext = (EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*))dlsym(egl, "eglCreateContext");
    auto queryString = (const char* (*)(EGLDisplay, EGLint))dlsym(egl, "eglQueryString");
    eglMakeCurrent_ = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))dlsym(egl, "eglMakeCurrent");
    eglDestroyContext_ = (EGLBoolean (*)(EGLDisplay, EGLContext))dlsym(egl, "eglDestroyContext");
    eglTerminate_ = (EGLBoolean (*)(EGLDisplay))dlsym(egl, "eglTerminate");
    if (!getProc || !getPlatformDisplay || !initialize || !bindAPI || !createContext || !eglMakeCurrent_)
    {
        fprintf(stderr, "gl: libEGL is missing EGL 1.5 calls\n");
        return false;
    }
    dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA_, nullptr, nullptr);
    EGLint major = 0, minor = 0;
    if (!dpy || !initialize(dpy, &major, &minor)) { fprintf(stderr, "gl: no EGL display\n"); return false; }
    if (!bindAPI(EGL_OPENGL_API_)) { fprintf(stderr, "gl: no desktop GL\n"); return false; }
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION_, 3, EGL_CONTEXT_MINOR_VERSION_, 2,
                            EGL_CONTEXT_OPENGL_PROFILE_MASK_, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_, EGL_NONE_};
    // no config (EGL_KHR_no_config_context) and no surface (EGL_KHR_surfaceless_context)
    ctx = createContext(dpy, nullptr, nullptr, attrs);
    if (!ctx || !eglMakeCurrent_(dpy, nullptr, nullptr, ctx)) { fprintf(stderr, "gl: no GL 3.2 core context\n"); return false; }
    if (!gladLoadGLLoader((GLADloadproc)getProc)) { fprintf(stderr, "gl: loading GL failed\n"); return false; }
    fprintf(stderr, "gl: EGL %d.%d (%s), %s, %s\n", major, minor, queryString ? queryString(dpy, EGL_VENDOR_) : "?",
            (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    glGenFramebuffers(1, &readFB);
    return true;
}

void GLContextClose()
{
    if (!ctx) return;
    eglMakeCurrent_(dpy, nullptr, nullptr, nullptr);
    eglDestroyContext_(dpy, ctx);
    eglTerminate_(dpy);
    ctx = nullptr;
}

void GLReadScreens(const void* output, uint32_t* top, uint32_t* bottom, bool late)
{
    // an array texture: layer 0 the top screen, layer 1 the bottom one (RGBA8, 1x)
    static const GLsizeiptr screen = 256 * 192 * 4;
    static GLuint pbo[2];
    static int cur = 0;
    static bool havePrev = false;
    GLuint tex = *(const GLuint*)output;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (late)
    {
        // into a buffer the GPU fills when it gets there; the frame before's is ready by now
        if (!pbo[0])
        {
            glGenBuffers(2, pbo);
            for (GLuint b : pbo)
            {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
                glBufferData(GL_PIXEL_PACK_BUFFER, 2 * screen, nullptr, GL_STREAM_READ);
            }
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[cur]);
    }
    for (int layer = 0; layer < 2; layer++)
    {
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, layer);
        if (late)
            glReadPixels(0, 0, 256, 192, GL_BGRA, GL_UNSIGNED_BYTE, (void*)(layer * screen));
        else
            glReadPixels(0, 0, 256, 192, GL_BGRA, GL_UNSIGNED_BYTE, layer ? bottom : top);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (!late) return;
    cur ^= 1;
    if (havePrev)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[cur]);
        if (const uint8_t* p = (const uint8_t*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 2 * screen, GL_MAP_READ_BIT))
        {
            memcpy(top, p, screen);
            memcpy(bottom, p + screen, screen);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    havePrev = true;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}
