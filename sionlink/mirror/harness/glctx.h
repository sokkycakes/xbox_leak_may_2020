// melonDS's OpenGL renderer (--renderer gl) without a window: an EGL context with no
// surface (Mesa's surfaceless platform, which finds the GPU's render node itself; in
// the cloud it falls back to software). libEGL is opened at run time, so a binary built
// with this still starts where there's no GL and keeps the software renderer.
#ifndef MIRROR_GLCTX_H
#define MIRROR_GLCTX_H

#include <cstdint>

// makes a GL 3.2 core context current on this thread and loads GL; false if it can't
bool GLContextOpen();
void GLContextClose();
// the GL renderer's output (the texture GetFramebuffers gives) as two XRGB screens.
// late: without waiting for the GPU, the frame before's (this one's comes next call)
void GLReadScreens(const void* output, uint32_t* top, uint32_t* bottom, bool late = false);
// ms spent getting late pictures (mapping the buffer: the GPU not done yet) since the last call
double GLTakeWaitMs();

#endif
