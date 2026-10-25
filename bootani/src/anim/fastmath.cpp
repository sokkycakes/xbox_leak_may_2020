//
//  fastmath.cpp
//
//  Portable replacement for the Xbox SDK FastMath library. The original
//  implemented these with hand-written x87/SSE assembly; each one had a
//  USE_C path equivalent to the plain C library call used here.
//
#include "precomp.h"
#include <math.h>

float fast_atan(float x)            { return atanf(x); }
float fast_atan2(float x, float y)  { return atan2f(x, y); }
float fast_acos(float x)            { return acosf(x); }
float fast_asin(float x)            { return asinf(x); }
float fast_log(float x)             { return logf(x); }
float fast_log10(float x)           { return log10f(x); }
float fast_exp(float x)             { return expf(x); }
float fast_sqrt(float x)            { return sqrtf(x); }
float fast_inversesqrt(float x)     { return 1.0f / sqrtf(x); }
float fast_fabs(float x)            { return fabsf(x); }
float fast_sin(float x)             { return sinf(x); }
float fast_cos(float x)             { return cosf(x); }
float fast_tan(float x)             { return tanf(x); }
float fast_pow(float x, float y)    { return powf(x, y); }
float fast_hypot(float x, float y)  { return hypotf(x, y); }
float fast_ceil(float x)            { return ceilf(x); }
float fast_floor(float x)           { return floorf(x); }
float fast_tanh(float x)            { return tanhf(x); }
float fast_cosh(float x)            { return coshf(x); }
float fast_sinh(float x)            { return sinhf(x); }

void fast_sincos(float x, SinCosPair* v)
{
    v->fSin = sinf(x);
    v->fCos = cosf(x);
}
