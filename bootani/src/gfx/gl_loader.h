//
//  gl_loader.h
//
//  A tiny OpenGL 3.3 core function loader: exactly the entry points the
//  D3D8 layer uses, resolved through the platform's GetProcAddress.
//
#ifndef BOOTANI_GL_LOADER_H
#define BOOTANI_GL_LOADER_H

// glcorearb.h pulls in <windows.h> on Windows unless APIENTRY is already
// defined; its Win32 typedefs would clash with the Xbox ones in xbox_compat.h.
#if defined(_WIN32) && !defined(APIENTRY)
#define APIENTRY __stdcall
#endif
#include <GL/glcorearb.h>

#define BOOTANI_GL_FUNCS(X) \
    X(PFNGLGETSTRINGPROC, glGetString) \
    X(PFNGLGETERRORPROC, glGetError) \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
    X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLCLEARPROC, glClear) \
    X(PFNGLCLEARCOLORPROC, glClearColor) \
    X(PFNGLCLEARDEPTHPROC, glClearDepth) \
    X(PFNGLCLEARSTENCILPROC, glClearStencil) \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLCOLORMASKPROC, glColorMask) \
    X(PFNGLBLENDFUNCPROC, glBlendFunc) \
    X(PFNGLCULLFACEPROC, glCullFace) \
    X(PFNGLFRONTFACEPROC, glFrontFace) \
    X(PFNGLPOLYGONMODEPROC, glPolygonMode) \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLTEXPARAMETERIVPROC, glTexParameteriv) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLFINISHPROC, glFinish) \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers) \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers) \
    X(PFNGLBINDSAMPLERPROC, glBindSampler) \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) \
    X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf) \
    X(PFNGLSAMPLERPARAMETERFVPROC, glSamplerParameterfv) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLVERTEXATTRIB4FPROC, glVertexAttrib4f) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) \
    X(PFNGLBINDFRAGDATALOCATIONPROC, glBindFragDataLocation) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM1FPROC, glUniform1f) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLUNIFORM2FVPROC, glUniform2fv) \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv) \
    X(PFNGLUNIFORM1IVPROC, glUniform1iv) \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
    X(PFNGLDRAWBUFFERPROC, glDrawBuffer) \
    X(PFNGLREADBUFFERPROC, glReadBuffer) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) \
    X(PFNGLUNIFORM3IVPROC, glUniform3iv) \
    X(PFNGLDEPTHRANGEPROC, glDepthRange)

#define BOOTANI_GL_DECLARE(type, name) extern type bootani_##name;
BOOTANI_GL_FUNCS(BOOTANI_GL_DECLARE)
#undef BOOTANI_GL_DECLARE

// Route the plain names to our pointers inside the gfx layer.
#include "gl_names.h"

typedef void* (*BootaniGetProcAddress)(const char* name);

// Returns false (and prints the missing entry point) if anything failed.
bool bootani_gl_load(BootaniGetProcAddress get_proc);

#endif // BOOTANI_GL_LOADER_H
