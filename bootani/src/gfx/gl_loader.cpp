//
//  gl_loader.cpp
//
#include "gl_loader.h"
#include <stdio.h>

#define BOOTANI_GL_DEFINE(type, name) type bootani_##name = 0;
BOOTANI_GL_FUNCS(BOOTANI_GL_DEFINE)
#undef BOOTANI_GL_DEFINE

bool bootani_gl_load(BootaniGetProcAddress get_proc)
{
    bool ok = true;
#define BOOTANI_GL_RESOLVE(type, name)                                  \
    bootani_##name = (type)get_proc(#name);                             \
    if (!bootani_##name) { fprintf(stderr, "bootani: missing GL entry point %s\n", #name); ok = false; }
    BOOTANI_GL_FUNCS(BOOTANI_GL_RESOLVE)
#undef BOOTANI_GL_RESOLVE
    return ok;
}
