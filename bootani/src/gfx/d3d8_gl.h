//
//  d3d8_gl.h
//
//  Host-side hooks of the D3D8-on-OpenGL layer (not part of the D3D API the
//  animation sees).
//
#ifndef BOOTANI_D3D8_GL_H
#define BOOTANI_D3D8_GL_H

#include "gl_loader.h"

namespace bootani_gl {

// Called by IDirect3DDevice8::Present with the framebuffer holding the
// finished frame. Row 0 of that framebuffer is the TOP of the image (D3D
// layout), so a blit to a window must flip it.
typedef void (*PresentHook)(GLuint fbo, int width, int height, void* user);
void SetPresentHook(PresentHook hook, void* user);

// Multisample count for the back buffer (1 = off). The Xbox used 2x
// horizontal supersampling; 4 is a good portable equivalent.
void SetBackBufferSamples(int samples);

} // namespace bootani_gl

#endif
