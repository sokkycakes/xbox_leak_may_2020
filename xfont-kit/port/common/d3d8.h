/* The library sources include <d3d8.h>; route it to the Xbox stand-in.
   Only XFONT's own sources include this name, so the min/max macros they
   expect from windows.h (C++ builds of it leave them out) stay private. */
#pragma once
#include "d3d8-xbox.h"
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
