//
//  shaders_glsl.cpp
//
//  GLSL 3.30 translations of the boot animation's NV2A shaders. The vertex
//  shaders are line-by-line ports of ani2/shaders/*.vsh (xvs.1.1); the pixel
//  shaders emulate the register-combiner semantics of *.psh (xps.1.1):
//    - texture, colour and constant inputs are unsigned [0,1] values,
//    - pixel shader constants are 8-bit colours on the NV2A, so clamp to [0,1],
//    - general-combiner results are clamped to [-1,1],
//    - _bx2 is 2x-1, "1-x" is 1-clamp(x,0,1), _sat clamps to [0,1],
//    - the final combiner (xfc A,B,C,D,E,F,G) is rgb = A*B + (1-A)*C + D with
//      E*F available as "prod", every input clamped to [0,1], alpha = G.
//
//  Interface shared by every stage so any vertex shader links with any pixel
//  shader: vertex attributes v0..v4 at locations 0..4, vertex constants c[],
//  outputs vD0 vD1 (colours) and vT0..vT3 (texture coordinates).
//
#include "shaders_glsl.h"
#include <string.h>

namespace bootani_gl {

//------------------------------------------------------------------------------
// Vertex side

static const char* kVSPrelude =
"#version 330 core\n"
"layout(location=0) in vec4 v0;\n"
"layout(location=1) in vec4 v1;\n"
"layout(location=2) in vec4 v2;\n"
"layout(location=3) in vec4 v3;\n"
"layout(location=4) in vec4 v4;\n"
"uniform vec4 c[96];\n"
"out vec4 vD0; out vec4 vD1; out vec4 vT0; out vec4 vT1; out vec4 vT2; out vec4 vT3;\n"
"float rcc(float x) {\n"
"    float r = 1.0 / x;\n"
"    return (r >= 0.0) ? clamp(r, 5.42101e-20, 1.884467e+19) : clamp(r, -1.884467e+19, -5.42101e-20);\n"
"}\n"
"float rsq(float x) { return inversesqrt(abs(x)); }\n"
"void xbox_init() {\n"
"    vD0 = vec4(0.0, 0.0, 0.0, 1.0); vD1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
"    vT0 = vec4(0.0, 0.0, 0.0, 1.0); vT1 = vT0; vT2 = vT0; vT3 = vT0;\n"
"}\n"
// D3D clip space -> GL clip space. y is negated so render targets are laid
// out top row first, exactly like D3D surfaces (see d3d8_gl.cpp); z goes
// from [0,w] to [-w,w].
"void xbox_emit(vec4 p) { gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w); }\n";

// scene_phong.vsh / scene_bump.vsh share everything but the vertex layout.
#define SCENE_LIGHTING_VS(S, T, N) \
"    vT3.x = dot(v0, c[8]);\n" \
"    vT3.y = dot(v0, c[9]);\n" \
"    vT3.z = dot(v0, c[10]);\n" \
"    vT3.w = max(dot(v0, c[11]), 0.0);\n" \
"    vec3 r8 = v0.xyz * c[6].xyz;\n" \
"    vec3 r0 = c[4].xyz - r8;\n" \
"    float d2 = dot(r0, r0);\n" \
"    float invd = rsq(d2);\n" \
"    vec3 r6 = vec3(1.0, d2 * invd, d2);\n" \
"    vD0.xyz = vec3(rcc(dot(r6, c[7].xyz)));\n" \
"    vec3 r1 = invd * r0;\n" \
"    vT1.xyz = vec3(dot(r1, " S ".xyz), dot(r1, " T ".xyz), dot(r1, " N ".xyz));\n" \
"    vec3 r3 = c[5].xyz - r8;\n" \
"    vec3 r2 = rsq(dot(r3, r3)) * r3;\n" \
"    vec3 h = r2 + r1;\n" \
"    vT2.xyz = vec3(dot(h, " S ".xyz), dot(h, " T ".xyz), dot(h, " N ".xyz));\n"

static const char* kVS_scene_phong =
"void main() {\n"
"    xbox_init();\n"
"    vec4 oPos = vec4(dot(v0, c[0]), dot(v0, c[1]), dot(v0, c[2]), dot(v0, c[3]));\n"
SCENE_LIGHTING_VS("v1", "v2", "v3")
"    xbox_emit(oPos);\n"
"}\n";

static const char* kVS_scene_bump =
"void main() {\n"
"    xbox_init();\n"
"    vec4 oPos = vec4(dot(v0, c[0]), dot(v0, c[1]), dot(v0, c[2]), dot(v0, c[3]));\n"
SCENE_LIGHTING_VS("v2", "v3", "v4")
"    vT0 = v1;\n"
"    xbox_emit(oPos);\n"
"}\n";

// scene_zr.vsh: depth-to-colour pass used by the green fog.
static const char* kVS_scene_zr =
"void main() {\n"
"    xbox_init();\n"
"    vec4 r4 = vec4(dot(v0, c[0]), dot(v0, c[1]), dot(v0, c[2]), dot(v0, c[3]));\n"
"    vec4 r10 = r4.zzzz;\n"
"    vec4 r8 = r10 + c[17];\n"
"    vec4 r0 = vec4(c[16].xyz * r8.xyz, 1.0);\n"
"    vD0 = max(r0, vec4(0.0, 0.0, 0.0, 1.0));\n"
"    float r9 = 1.0 / r4.w;\n"
"    vec4 r3 = (r4 * r9) * c[18];\n"
"    vT0 = r3 + c[19];\n"
"    vT0.w = 1.0;\n"
"    xbox_emit(r4);\n"
"}\n";

// greenfog.vsh
static const char* kVS_greenfog =
"void main() {\n"
"    xbox_init();\n"
"    vT0 = v1;\n"
"    vT1 = v2 * c[1] + c[0];\n"
"    vT2 = v2 * c[3] + c[2];\n"
"    vT3 = v2 * c[5] + c[4];\n"
"    xbox_emit(v0);\n"
"}\n";

// vblob.vsh
static const char* kVS_vblob =
"void main() {\n"
"    xbox_init();\n"
"    vec4 r10 = v0 * c[11];\n"
"    vec4 en = vec4(rsq(dot(r10.xyz, r10.xyz)) * r10.xyz, c[8].y);\n"
"    vec4 r11 = v0 * c[10] + c[12];\n"
"    vec4 wp = en * vec4(v1.www, c[8].x) + r11;\n"
"    vec3 e = c[9].xyz - wp.xyz;\n"
"    vT1 = vec4(rsq(dot(e, e)) * e, c[8].y);\n"
"    vec4 r0 = v1.xyzz * c[11];\n"
"    vT0 = vec4(rsq(dot(r0.xyz, r0.xyz)) * r0.xyz, c[8].y);\n"
"    xbox_emit(vec4(dot(wp, c[4]), dot(wp, c[5]), dot(wp, c[6]), dot(wp, c[7])));\n"
"}\n";

// vbloblet.vsh
static const char* kVS_vbloblet =
"void main() {\n"
"    xbox_init();\n"
"    vT0 = v0;\n"
"    vec4 r0 = vec4(dot(v0.xyz, c[11].xyz));\n"
"    vec4 r1 = r0 * c[13];\n"
"    vec4 r2 = c[12] * v0 + r1;\n"
"    vec4 wp = vec4((r2 + c[10]).xyz, c[8].y);\n"
"    vec3 e = c[9].xyz - wp.xyz;\n"
"    vT1 = vec4(rsq(dot(e, e)) * e, c[8].y);\n"
"    xbox_emit(vec4(dot(wp, c[4]), dot(wp, c[5]), dot(wp, c[6]), dot(wp, c[7])));\n"
"}\n";

// Fixed-function vertex pipeline for the FVFs the animation uses:
// XYZ[|TEX1|TEX2] with world/view/projection, XYZRHW|TEX1 pre-transformed.
static const char* kVS_fixed =
"uniform mat4 uWVP;\n"
"uniform int uRHW;\n"
"uniform vec4 uViewport;\n"      // x, y, width, height
"uniform int uHasDiffuse;\n"
"void main() {\n"
"    xbox_init();\n"
"    vec4 p;\n"
"    if (uRHW != 0) {\n"
"        float w = 1.0 / v0.w;\n"
"        vec2 ndc = vec2((v0.x - uViewport.x) / uViewport.z * 2.0 - 1.0,\n"
"                        1.0 - (v0.y - uViewport.y) / uViewport.w * 2.0);\n"
"        p = vec4(ndc * w, v0.z * w, w);\n"
"    } else {\n"
"        p = uWVP * vec4(v0.xyz, 1.0);\n"
"    }\n"
"    vD0 = (uHasDiffuse != 0) ? v3.bgra : vec4(1.0);\n"
"    vT0 = v1;\n"
"    vT1 = v2;\n"
"    xbox_emit(p);\n"
"}\n";

//------------------------------------------------------------------------------
// Pixel side

static const char* kPSPrelude =
"#version 330 core\n"
"in vec4 vD0; in vec4 vD1; in vec4 vT0; in vec4 vT1; in vec4 vT2; in vec4 vT3;\n"
"uniform vec4 pcRaw[8];\n"
"uniform vec2 texScale[4];\n"   // 1/size for Xbox linear textures, else 1
"uniform int uAlphaFunc;\n"     // 0 = alpha test off, else D3DCMP_*
"uniform float uAlphaRef;\n"
"layout(location=0) out vec4 fragColor;\n"
"vec4 pc(int i) { return clamp(pcRaw[i], 0.0, 1.0); }\n"
"vec4 cmb(vec4 x) { return clamp(x, -1.0, 1.0); }\n"
"vec4 bx2(vec4 x) { return 2.0 * clamp(x, 0.0, 1.0) - 1.0; }\n"
"vec4 inv(vec4 x) { return 1.0 - clamp(x, 0.0, 1.0); }\n"
"vec4 sat(vec4 x) { return clamp(x, 0.0, 1.0); }\n"
"float dp3(vec4 a, vec4 b) { return dot(a.xyz, b.xyz); }\n"
"vec2 proj2(vec4 t, int stage) { return (t.xy / t.w) * texScale[stage]; }\n"
"vec4 xfc(vec4 A, vec4 B, vec4 C, vec4 D, float G) {\n"
"    A = sat(A); B = sat(B); C = sat(C); D = sat(D);\n"
"    return vec4(sat(A * B + (1.0 - A) * C + D).rgb, clamp(G, 0.0, 1.0));\n"
"}\n"
"void xbox_output(vec4 color) {\n"
"    color = clamp(color, 0.0, 1.0);\n"
"    if (uAlphaFunc != 0) {\n"
"        float a = floor(color.a * 255.0 + 0.5); float r = uAlphaRef;\n"
"        bool pass = true;\n"
"        if (uAlphaFunc == 1) pass = false;\n"
"        else if (uAlphaFunc == 2) pass = a <  r;\n"
"        else if (uAlphaFunc == 3) pass = a == r;\n"
"        else if (uAlphaFunc == 4) pass = a <= r;\n"
"        else if (uAlphaFunc == 5) pass = a >  r;\n"
"        else if (uAlphaFunc == 6) pass = a != r;\n"
"        else if (uAlphaFunc == 7) pass = a >= r;\n"
"        if (!pass) discard;\n"
"    }\n"
"    fragColor = color;\n"
"}\n";

// scene_phong.psh / scene_bump.psh tail: specular power, light sum and the
// shadow-map final combiner.
#define SCENE_TAIL_PS \
"    vec4 c4 = vec4(0.25, 0.25, 0.25, 1.0);\n" \
"    vec4 v0 = sat(vD0);\n" \
"    vec4 d0 = cmb(pc(1) * sat(r0));\n" \
"    vec4 d1 = cmb(pc(2) * sat(r1));\n" \
"    vec4 v1 = cmb(d0 + d1);\n" \
"    r0.rgb = cmb(v0 * sat(v1) + pc(0)).rgb;\n" \
"    xbox_output(xfc(r0, t3, vec4(0.0), sat(r0) * c4, c4.a));\n"

static const char* kPS_scene_phong =
"uniform samplerCube tex1;\n"
"uniform samplerCube tex2;\n"
"uniform sampler2DShadow tex3;\n"
"void main() {\n"
"    vec4 N = vec4(0.0, 0.0, 1.0, 1.0);\n"
"    vec4 t1 = texture(tex1, vT1.xyz);\n"
"    vec4 t2 = texture(tex2, vT2.xyz);\n"
"    vec4 t3 = vec4(texture(tex3, vec3(proj2(vT3, 3), (vT3.z / vT3.w) / 65535.0)));\n"
"    vec4 r0 = cmb(vec4(dp3(N, bx2(t1))));\n"
"    vec4 r1 = cmb(vec4(dp3(N, bx2(t2))));\n"
"    r1.rgb = cmb(sat(r1) * sat(r1)).rgb;\n"
"    r1.rgb = cmb(r1 * r1).rgb;\n"
"    r1.rgb = cmb(r1 * r1).rgb;\n"
"    r1.rgb = cmb(r1 * r1).rgb;\n"
"    r1.rgb = cmb(r1 * r1).rgb;\n"
SCENE_TAIL_PS
"}\n";

static const char* kPS_scene_bump =
"uniform sampler2D tex0;\n"
"uniform samplerCube tex1;\n"
"uniform samplerCube tex2;\n"
"uniform sampler2DShadow tex3;\n"
"void main() {\n"
"    vec4 t0 = texture(tex0, proj2(vT0, 0));\n"
"    vec4 t1 = texture(tex1, vT1.xyz);\n"
"    vec4 t2 = texture(tex2, vT2.xyz);\n"
"    vec4 t3 = vec4(texture(tex3, vec3(proj2(vT3, 3), (vT3.z / vT3.w) / 65535.0)));\n"
"    vec4 r0 = cmb(vec4(dp3(bx2(t0), bx2(t1))));\n"
"    vec4 r1 = cmb(vec4(dp3(bx2(t0), bx2(t2))));\n"
"    r1.rgb = cmb(sat(r1) * sat(r1)).rgb;\n"
"    r1.rgb = cmb(r1 * r1).rgb;\n"
"    r1.rgb = cmb(r1 * r1).rgb;\n"
"    r1.rgb = cmb(r1 * r1).rgb;\n"
SCENE_TAIL_PS
"}\n";

// scene_zr.psh
static const char* kPS_scene_zr =
"void main() {\n"
"    vec4 t0 = sat(vT0);\n"          // texcoord t0
"    vec4 r1 = cmb(vec4(dp3(bx2(t0), bx2(t0))));\n"
"    r1 = cmb(inv(r1) * inv(r1));\n"
"    vec4 r0 = cmb(sat(vD0) * r1);\n"
"    xbox_output(xfc(vec4(0.0), vec4(0.0), vec4(0.0), r0, 1.0));\n"
"}\n";

// greenfog.psh
static const char* kPS_greenfog =
"uniform sampler2D tex0;\n"
"uniform sampler2D tex1;\n"
"uniform sampler2D tex2;\n"
"uniform sampler2D tex3;\n"
"void main() {\n"
"    vec4 t0 = texture(tex0, proj2(vT0, 0));\n"
"    vec4 t1 = texture(tex1, proj2(vT1, 1));\n"
"    vec4 t2 = texture(tex2, proj2(vT2, 2));\n"
"    vec4 t3 = texture(tex3, proj2(vT3, 3));\n"
"    vec4 r1 = cmb(vec4(t1.a + t2.a));\n"
"    r1 = cmb(r1 + t3.a);\n"
"    r1 = cmb(r1 * pc(0));\n"
"    vec4 r0;\n"
"    r0.rgb = cmb(t0 * r1).rgb;\n"
"    r0.rgb = cmb(r0 + pc(1)).rgb;\n"
"    r0.a = 1.0;\n"
"    xbox_output(r0);\n"
"}\n";

// vblob.psh
static const char* kPS_vblob =
"uniform samplerCube tex0;\n"
"uniform samplerCube tex1;\n"
"void main() {\n"
"    vec4 t0 = texture(tex0, vT0.xyz);\n"
"    vec4 t1 = texture(tex1, vT1.xyz);\n"
"    vec4 r0 = cmb(vec4(dp3(bx2(t0), bx2(t1))));\n"
"    vec4 r1 = sat(r0);\n"
"    r0 = cmb(inv(r1) * inv(r1));\n"
"    r0.rgb = cmb(inv(r0) * pc(0)).rgb;\n"
"    r0.rgb = cmb(r0 + pc(1)).rgb;\n"
"    r0.a = pc(0).a;\n"
"    xbox_output(r0);\n"
"}\n";

// vbloblet.psh
static const char* kPS_vbloblet =
"uniform samplerCube tex0;\n"
"uniform samplerCube tex1;\n"
"void main() {\n"
"    vec4 t0 = texture(tex0, vT0.xyz);\n"
"    vec4 t1 = texture(tex1, vT1.xyz);\n"
"    vec4 r0 = cmb(vec4(dp3(bx2(t0), bx2(t1))));\n"
"    vec4 r1 = sat(r0);\n"
"    r0 = cmb(inv(r1) * inv(r1));\n"
"    r0 = cmb(inv(r0) * pc(0));\n"
"    r0.rgb = cmb(r0 + pc(1)).rgb;\n"
"    r0.a = cmb(r0 * pc(2)).a;\n"
"    xbox_output(r0);\n"
"}\n";

// Fixed-function texture stage 0 (stage 1 is always disabled by the
// animation). Ops and args use the values from d3d8_compat.h.
static const char* kPS_fixed =
"uniform sampler2D tex0;\n"
"uniform int uHasTex0;\n"
"uniform ivec3 uColorOp;\n"     // op, arg1, arg2
"uniform ivec3 uAlphaOp;\n"
"uniform vec4 uTFactor;\n"
"vec4 arg(int a, vec4 tex) {\n"
"    if (a == 2) return tex;\n"     // D3DTA_TEXTURE
"    if (a == 3) return uTFactor;\n" // D3DTA_TFACTOR
"    return vD0;\n"                  // D3DTA_DIFFUSE / D3DTA_CURRENT on stage 0
"}\n"
"vec4 op(int o, vec4 a1, vec4 a2, vec4 cur) {\n"
"    if (o == 1) return cur;\n"                      // DISABLE
"    if (o == 2) return a1;\n"                       // SELECTARG1
"    if (o == 3) return a2;\n"                       // SELECTARG2
"    if (o == 4) return a1 * a2;\n"                  // MODULATE
"    if (o == 5) return clamp(2.0 * a1 * a2, 0.0, 1.0);\n" // MODULATE2X
"    if (o == 6) return clamp(a1 + a2, 0.0, 1.0);\n"  // ADD
"    return cur;\n"
"}\n"
"void main() {\n"
"    vec4 tex = (uHasTex0 != 0) ? texture(tex0, vT0.xy * texScale[0]) : vec4(1.0);\n"
"    vec4 cur = vD0;\n"
"    vec4 col = op(uColorOp.x, arg(uColorOp.y, tex), arg(uColorOp.z, tex), cur);\n"
"    vec4 alp = (uColorOp.x == 1) ? cur : op(uAlphaOp.x, arg(uAlphaOp.y, tex), arg(uAlphaOp.z, tex), cur);\n"
"    xbox_output(vec4(col.rgb, alp.a));\n"
"}\n";

#include "shaders_slash_interior.inc"

//------------------------------------------------------------------------------

struct ShaderEntry { const char* name; const char* body; };

static const ShaderEntry kVertexShaders[] = {
    { "scene_phong", kVS_scene_phong },
    { "scene_bump",  kVS_scene_bump  },
    { "scene_zr",    kVS_scene_zr    },
    { "greenfog",    kVS_greenfog    },
    { "vblob",       kVS_vblob       },
    { "vbloblet",    kVS_vbloblet    },
    { "slash_interior", kVS_slash_interior },
    { "fixed",       kVS_fixed       },
};

static const ShaderEntry kPixelShaders[] = {
    { "scene_phong", kPS_scene_phong },
    { "scene_bump",  kPS_scene_bump  },
    { "scene_zr",    kPS_scene_zr    },
    { "greenfog",    kPS_greenfog    },
    { "vblob",       kPS_vblob       },
    { "vbloblet",    kPS_vbloblet    },
    { "slash_interior", kPS_slash_interior },
    { "fixed",       kPS_fixed       },
};

const char* VertexShaderPrelude() { return kVSPrelude; }
const char* PixelShaderPrelude()  { return kPSPrelude; }

const char* FindVertexShader(const char* name)
{
    for (size_t i = 0; i < sizeof(kVertexShaders) / sizeof(kVertexShaders[0]); i++)
        if (!strcmp(kVertexShaders[i].name, name)) return kVertexShaders[i].body;
    return 0;
}

const char* FindPixelShader(const char* name)
{
    for (size_t i = 0; i < sizeof(kPixelShaders) / sizeof(kPixelShaders[0]); i++)
        if (!strcmp(kPixelShaders[i].name, name)) return kPixelShaders[i].body;
    return 0;
}

} // namespace bootani_gl
