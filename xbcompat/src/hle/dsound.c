/*
 * DirectSound (dsound.lib) on SDL2 audio.
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <string.h>

#include "../xbcompat.h"
#include "hle.h"

const struct hle_func dsound_funcs[] = {
    { NULL, NULL },
};
