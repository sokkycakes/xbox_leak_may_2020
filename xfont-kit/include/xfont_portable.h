/* Include this to use XFONT outside the Xbox SDK.  It pulls in the Win32
   basics (the real <windows.h> on Windows, port/posix/windows.h elsewhere),
   the Xbox D3D types XFONT needs, and the PC flavour of the XFONT API,
   including XFONT_OpenTrueTypeFont. */
#pragma once

#ifndef XFONT_TRUETYPE
#define XFONT_TRUETYPE
#endif

#include <windows.h>
#include "d3d8-xbox.h"
#include "xfont-pc.h"
#include "xfontformat-pc.h"
