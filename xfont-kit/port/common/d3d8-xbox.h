/* Stand-in for the Xbox d3d8.h.  The portable XFONT build only draws into
   memory (XFONT_TextOutToMemory); XFONT_TextOut on a D3D surface is Xbox
   only, so the surface type stays opaque. */
#pragma once
#include "d3d8types-xbox.h"
typedef struct IDirect3DSurface8 IDirect3DSurface8;
