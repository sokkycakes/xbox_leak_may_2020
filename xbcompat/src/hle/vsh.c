/* NV2A vertex program → GLSL translator: see nv2a_shaders.h. */
#include <stdlib.h>
#include "nv2a_shaders.h"

__attribute__((weak)) char *vsh_translate(const uint32_t *code, unsigned count)
{
    (void)code; (void)count;
    return NULL;
}
