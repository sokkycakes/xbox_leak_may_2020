/* floating point workloads (compiled for x87 and for SSE): 4x4 transforms,
   vector normalization, n-body integration */
#include "rt.h"
typedef struct { float m[4][4]; } mat4;
typedef struct { float x, y, z, w; } vec4;
static float fsqrt(float x)
{
#if defined(__SSE__) && !defined(USE_X87)
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    float r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
#endif
}
static double dsqrt(double x)
{
    double r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}
static mat4 mul(const mat4 *a, const mat4 *b)
{
    mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] +
                        a->m[i][3] * b->m[3][j];
    return r;
}
static vec4 xform(const mat4 *m, vec4 v)
{
    vec4 r;
    r.x = m->m[0][0] * v.x + m->m[1][0] * v.y + m->m[2][0] * v.z + m->m[3][0] * v.w;
    r.y = m->m[0][1] * v.x + m->m[1][1] * v.y + m->m[2][1] * v.z + m->m[3][1] * v.w;
    r.z = m->m[0][2] * v.x + m->m[1][2] * v.y + m->m[2][2] * v.z + m->m[3][2] * v.w;
    r.w = m->m[0][3] * v.x + m->m[1][3] * v.y + m->m[2][3] * v.z + m->m[3][3] * v.w;
    return r;
}
static vec4 verts[4096];
typedef struct { double x, y, z, vx, vy, vz, m; } body;
int bench(void)
{
    mat4 a, b;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            a.m[i][j] = (float)(i * 4 + j) * 0.01f + 0.5f;
            b.m[i][j] = (i == j) ? 0.99f : 0.001f * (float)(i - j);
        }
    for (int k = 0; k < 3000; k++)
        a = mul(&a, &b);
    float sum = 0;
    for (int i = 0; i < 4096; i++) {
        verts[i].x = (float)(i & 63) - 32.0f;
        verts[i].y = (float)(i >> 6) - 32.0f;
        verts[i].z = (float)(i % 7);
        verts[i].w = 1.0f;
    }
    for (int rep = 0; rep < 12; rep++)
        for (int i = 0; i < 4096; i++) {
            vec4 v = xform(&b, verts[i]);
            float len = fsqrt(v.x * v.x + v.y * v.y + v.z * v.z) + 1e-6f;
            v.x /= len; v.y /= len; v.z /= len;
            verts[i] = v;
            sum += v.x - v.y + (v.z > 0.5f ? 1.0f : 0.0f);
        }
    body bd[5] = { { 0, 0, 0, 0, 0, 0, 39.47 }, { 4.84, -1.16, -0.10, 0.60, 2.81, -0.02, 0.037 },
                   { 8.34, 4.12, -0.40, -1.01, 1.82, 0.008, 0.011 }, { 12.89, -15.11, -0.22, 1.08, 0.86, -0.01, 0.0017 },
                   { 15.37, -25.91, 0.17, 0.97, 0.59, -0.03, 0.002 } };
    for (int step = 0; step < 6000; step++) {
        for (int i = 0; i < 5; i++)
            for (int j = i + 1; j < 5; j++) {
                double dx = bd[i].x - bd[j].x, dy = bd[i].y - bd[j].y, dz = bd[i].z - bd[j].z;
                double d2 = dx * dx + dy * dy + dz * dz, mag = 0.01 / (d2 * dsqrt(d2));
                bd[i].vx -= dx * bd[j].m * mag; bd[i].vy -= dy * bd[j].m * mag; bd[i].vz -= dz * bd[j].m * mag;
                bd[j].vx += dx * bd[i].m * mag; bd[j].vy += dy * bd[i].m * mag; bd[j].vz += dz * bd[i].m * mag;
            }
        for (int i = 0; i < 5; i++) {
            bd[i].x += 0.01 * bd[i].vx; bd[i].y += 0.01 * bd[i].vy; bd[i].z += 0.01 * bd[i].vz;
        }
    }
    double e = 0;
    for (int i = 0; i < 5; i++) e += bd[i].x + bd[i].vy;
    u32 r = 0;
    r ^= (u32)(int)(a.m[1][2] * 1000.0f);
    r ^= (u32)(int)(sum * 100.0f) << 3;
    r ^= (u32)(int)(e * 1000.0);
    return (int)r;
}
