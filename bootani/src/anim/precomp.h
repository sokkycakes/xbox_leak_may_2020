//
//  precomp.h
//
//  Portable replacement for the original precompiled header, which pulled in
//  the NT kernel headers, xtl.h and xgraphics.h. The animation now sees the
//  D3D8 subset from compat/ and allocates from the C heap.
//
#pragma once

#include "../compat/d3d8_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

inline void* MemAlloc(UINT cBytes)
{
    // The original zeroed every allocation (see the "start-up animation
    // hangs" note in ani2/precomp.h); keep that.
    return calloc(1, cBytes);
}

inline void* MemAllocNoZero(ULONG cBytes)
{
    return MemAlloc(cBytes);
}

inline void MemFree(void* pv)
{
    free(pv);
}

#include "shaders.h"
#include "fastmath.h"

// Platform services the animation needs (implemented by the host program).
DWORD Bootani_GetTickCount();
void  Bootani_AudioStart();
void  Bootani_AudioStop();
void  Bootani_AudioRestart();
