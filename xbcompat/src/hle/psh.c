/*
 * NV2A register combiners (Xbox pixel shaders) to GLSL 1.20.
 *
 * The input is the D3DPIXELSHADERDEF as the Xbox D3D library leaves it in
 * D3D__RenderState (see nv2a_shaders.h for which entries we may read).  The
 * library pushes those dwords straight into the NV2A combiner registers, so
 * the bit layouts here are the PS_* macros of d3d8types.h, which are also
 * the hardware encodings.
 *
 * Model (NV_register_combiners / NV_texture_shader):
 *   - four texture shader stages produce t0..t3 (and the dot-product
 *     results the dependent modes chain through),
 *   - up to eight general combiner stages, each an RGB and an alpha half
 *     computing AB, CD and AB+CD / mux(AB, CD) from four mapped inputs
 *     and writing them (scaled/biased, clamped to -1..1) to R0, R1, the
 *     texture or colour registers,
 *   - the final combiner: rgb = A*B + (1-A)*C + D, alpha = G, with the
 *     V1+R0 sum and E*F product specials.
 * When a shader leaves both final combiner words zero the library does not
 * touch the hardware's final combiner, which then still holds the
 * fixed-function setup from LazySetSpecFogCombiner: R0 out, blended with the
 * fog colour by the fog factor when D3DRS_FOGENABLE is set.  (The
 * fixed-function path also adds specular when D3DRS_SPECULARENABLE is set;
 * that render state is not part of our input, so it is not applied.)
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_shaders.h"

/* ---- render state slots we read ---------------------------------------- */
#define RS_PSALPHAINPUTS0        0
#define RS_PSFINALCOMBINERABCD   8
#define RS_PSFINALCOMBINEREFG    9
#define RS_PSCONSTANT0_0        10
#define RS_PSCONSTANT1_0        18
#define RS_PSALPHAOUTPUTS0      26
#define RS_PSRGBINPUTS0         34
#define RS_PSCOMPAREMODE        42
#define RS_PSRGBOUTPUTS0        45
#define RS_PSCOMBINERCOUNT      53
#define RS_PSDOTMAPPING         55
#define RS_PSINPUTTEXTURE       56
#define RS_FOGENABLE            82
#define RS_FOGTABLEMODE         83
#define RS_PSTEXTUREMODES      117

/* ---- PS_* encodings (d3d8types.h) -------------------------------------- */
enum {
    REG_ZERO = 0, REG_C0 = 1, REG_C1 = 2, REG_FOG = 3, REG_V0 = 4, REG_V1 = 5,
    REG_T0 = 8, REG_T1 = 9, REG_T2 = 10, REG_T3 = 11, REG_R0 = 12, REG_R1 = 13,
    REG_V1R0_SUM = 14, REG_EF_PROD = 15,
};

enum {
    MAP_UNSIGNED_IDENTITY = 0x00, MAP_UNSIGNED_INVERT = 0x20,
    MAP_EXPAND_NORMAL = 0x40, MAP_EXPAND_NEGATE = 0x60,
    MAP_HALFBIAS_NORMAL = 0x80, MAP_HALFBIAS_NEGATE = 0xa0,
    MAP_SIGNED_IDENTITY = 0xc0, MAP_SIGNED_NEGATE = 0xe0,
};

enum { CHAN_RGB_OR_BLUE = 0x00, CHAN_ALPHA = 0x10 };

enum {
    OUT_IDENTITY = 0x00, OUT_BIAS = 0x08, OUT_SHIFTLEFT_1 = 0x10,
    OUT_SHIFTLEFT_1_BIAS = 0x18, OUT_SHIFTLEFT_2 = 0x20, OUT_SHIFTRIGHT_1 = 0x30,
};

enum {
    TM_NONE = 0x00, TM_PROJECT2D, TM_PROJECT3D, TM_CUBEMAP, TM_PASSTHRU,
    TM_CLIPPLANE, TM_BUMPENVMAP, TM_BUMPENVMAP_LUM, TM_BRDF, TM_DOT_ST,
    TM_DOT_ZW, TM_DOT_RFLCT_DIFF, TM_DOT_RFLCT_SPEC, TM_DOT_STR_3D,
    TM_DOT_STR_CUBE, TM_DPNDNT_AR, TM_DPNDNT_GB, TM_DOTPRODUCT,
    TM_DOT_RFLCT_SPEC_CONST,
};

enum { FCS_CLAMP_SUM = 0x80, FCS_COMPLEMENT_V1 = 0x40, FCS_COMPLEMENT_R0 = 0x20 };

enum { CC_MUX_MSB = 0x1, CC_UNIQUE_C0 = 0x10, CC_UNIQUE_C1 = 0x100 };

/* Screen-space z range of the common 24-bit depth buffer: the value the
   DOT_ZW quotient is expressed in on the Xbox (D3DZ_MAX_D24S8). */
#define ZW_DEPTH_SCALE "16777215.0"

/* ---- string builder ---------------------------------------------------- */
typedef struct {
    char *s;
    size_t len, cap;
} sbuf;

static void sb_init(sbuf *b)
{
    b->cap = 1024;
    b->len = 0;
    b->s = malloc(b->cap);
    b->s[0] = 0;
}

static void sb_free(sbuf *b)
{
    free(b->s);
    b->s = NULL;
}

static void sb_cat(sbuf *b, const char *str)
{
    size_t n = strlen(str);
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, str, n + 1);
    b->len += n;
}

static void sb_fmt(sbuf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_cat(b, tmp);
}

/* ---- parsed shader ----------------------------------------------------- */
typedef struct {
    int reg, chan, map;
} psh_input;

typedef struct {
    int ab, cd, sum;         /* destination registers (0 = discard) */
    int ab_dot, cd_dot, mux; /* AB / CD dot product, SUM is a mux */
    int mapping;             /* OUT_* */
    int ab_b2a, cd_b2a;      /* blue to alpha (RGB half only) */
} psh_output;

typedef struct {
    const uint32_t *rs;
    int nstages, flags;
    int texmode[4], inputtex[4], dotmap[4], compare[4];

    /* usage discovered while generating */
    int sampler[4];          /* 0 none, 2 = sampler2D, 3 = sampler3D, 6 = samplerCube */
    int use_texscale, use_c0, use_c1, use_fc0, use_fc1, use_bumpenv, use_bumplum;
    int use_fog, use_v1r0, use_efprod, use_frag_depth;
    int use_snorm8[3];       /* snorm8_d3d, snorm8_gl, snorm8 */
    int use_hilo16, use_snorm16[3], use_hemi;
    int use_dotmap[8];
    int dot_defined[4];

    sbuf tex;                /* texture shader stage code */
    sbuf comb;               /* combiner code */
} psh_ctx;

static void parse_input(psh_input *in, uint32_t v)
{
    in->reg = v & 0xf;
    in->chan = v & 0x10;
    in->map = v & 0xe0;
}

static void parse_inputs(uint32_t v, psh_input *a, psh_input *b, psh_input *c, psh_input *d)
{
    parse_input(a, v >> 24);
    parse_input(b, v >> 16);
    parse_input(c, v >> 8);
    parse_input(d, v);
}

static void parse_output(psh_output *o, uint32_t v)
{
    unsigned flags = v >> 12;
    o->cd = v & 0xf;
    o->ab = (v >> 4) & 0xf;
    o->sum = (v >> 8) & 0xf;
    o->cd_dot = flags & 1;
    o->ab_dot = flags & 2;
    o->mux = flags & 4;
    o->mapping = flags & 0x38;
    o->cd_b2a = flags & 0x40;
    o->ab_b2a = flags & 0x80;
}

/* ---- register / input expressions -------------------------------------- */

/* The GLSL name of a readable register.  `where`: 0 = general stage
   `stage`, 1 = final combiner A..D, 2 = final combiner E/F/G. */
static void reg_name(psh_ctx *c, char *out, size_t n, int reg, int where, int stage)
{
    switch (reg) {
    case REG_C0:
        if (where) { c->use_fc0 = 1; snprintf(out, n, "fc0"); }
        else { c->use_c0 = 1; snprintf(out, n, "c0[%d]", (c->flags & CC_UNIQUE_C0) ? stage : 0); }
        break;
    case REG_C1:
        if (where) { c->use_fc1 = 1; snprintf(out, n, "fc1"); }
        else { c->use_c1 = 1; snprintf(out, n, "c1[%d]", (c->flags & CC_UNIQUE_C1) ? stage : 0); }
        break;
    case REG_FOG: c->use_fog = 1; snprintf(out, n, "fog"); break;
    case REG_V0: snprintf(out, n, "v0"); break;
    case REG_V1: snprintf(out, n, "v1"); break;
    case REG_T0: case REG_T1: case REG_T2: case REG_T3:
        snprintf(out, n, "t%d", reg - REG_T0);
        break;
    case REG_R0: snprintf(out, n, "r0"); break;
    case REG_R1: snprintf(out, n, "r1"); break;
    case REG_V1R0_SUM:
        if (where == 1) { c->use_v1r0 = 1; snprintf(out, n, "v1r0"); }
        else snprintf(out, n, "vec4(0.0)");
        break;
    case REG_EF_PROD:
        if (where == 1) { c->use_efprod = 1; snprintf(out, n, "efprod"); }
        else snprintf(out, n, "vec4(0.0)");
        break;
    default: /* ZERO and the two unassigned numbers */
        snprintf(out, n, "vec4(0.0)");
        break;
    }
}

/* A mapped input: `vec3` for the RGB half (and the final combiner's colour
   inputs), `float` for the alpha half. */
static void input_expr(psh_ctx *c, char *out, size_t n, psh_input in, int is_alpha, int where, int stage)
{
    static const char *zero_vec[8] = { "vec3(0.0)", "vec3(1.0)", "vec3(-1.0)", "vec3(1.0)",
                                       "vec3(-0.5)", "vec3(0.5)", "vec3(0.0)", "vec3(0.0)" };
    static const char *zero_scl[8] = { "0.0", "1.0", "-1.0", "1.0", "-0.5", "0.5", "0.0", "0.0" };
    char reg[32], x[64];

    if (in.reg == REG_ZERO || ((in.reg == REG_V1R0_SUM || in.reg == REG_EF_PROD) && where != 1)
        || in.reg == 6 || in.reg == 7) {
        snprintf(out, n, "%s", is_alpha ? zero_scl[in.map >> 5] : zero_vec[in.map >> 5]);
        return;
    }
    reg_name(c, reg, sizeof reg, in.reg, where, stage);
    snprintf(x, sizeof x, "%s%s", reg,
             is_alpha ? (in.chan ? ".a" : ".b") : (in.chan ? ".aaa" : ".rgb"));
    switch (in.map) {
    case MAP_UNSIGNED_INVERT:  snprintf(out, n, "(1.0 - clamp(%s, 0.0, 1.0))", x); break;
    case MAP_EXPAND_NORMAL:    snprintf(out, n, "(2.0 * max(%s, 0.0) - 1.0)", x); break;
    case MAP_EXPAND_NEGATE:    snprintf(out, n, "(1.0 - 2.0 * max(%s, 0.0))", x); break;
    case MAP_HALFBIAS_NORMAL:  snprintf(out, n, "(max(%s, 0.0) - 0.5)", x); break;
    case MAP_HALFBIAS_NEGATE:  snprintf(out, n, "(0.5 - max(%s, 0.0))", x); break;
    case MAP_SIGNED_IDENTITY:  snprintf(out, n, "%s", x); break;
    case MAP_SIGNED_NEGATE:    snprintf(out, n, "(-%s)", x); break;
    default:                   snprintf(out, n, "max(%s, 0.0)", x); break;
    }
}

/* Output scale / bias, then the clamp every combiner output gets. */
static void output_expr(char *out, size_t n, const char *x, int mapping)
{
    switch (mapping) {
    case OUT_BIAS:             snprintf(out, n, "clamp(%s - 0.5, -1.0, 1.0)", x); break;
    case OUT_SHIFTLEFT_1:      snprintf(out, n, "clamp(%s * 2.0, -1.0, 1.0)", x); break;
    case OUT_SHIFTLEFT_1_BIAS: snprintf(out, n, "clamp((%s - 0.5) * 2.0, -1.0, 1.0)", x); break;
    case OUT_SHIFTLEFT_2:      snprintf(out, n, "clamp(%s * 4.0, -1.0, 1.0)", x); break;
    case OUT_SHIFTRIGHT_1:     snprintf(out, n, "clamp(%s * 0.5, -1.0, 1.0)", x); break;
    default:                   snprintf(out, n, "clamp(%s, -1.0, 1.0)", x); break;
    }
}

/* Writable registers: R0, R1, V0, V1, T0..T3.  Anything else is a discard. */
static const char *dest_name(int reg)
{
    switch (reg) {
    case REG_V0: return "v0";
    case REG_V1: return "v1";
    case REG_T0: return "t0";
    case REG_T1: return "t1";
    case REG_T2: return "t2";
    case REG_T3: return "t3";
    case REG_R0: return "r0";
    case REG_R1: return "r1";
    default: return NULL;
    }
}

/* ---- texture shader stages --------------------------------------------- */

static const char *dotmap_func(psh_ctx *c, int stage)
{
    static const char *names[8] = {
        "dotmap_unsigned", "dotmap_snorm_d3d", "dotmap_snorm_gl", "dotmap_snorm",
        "dotmap_hilo_1", "dotmap_hilo_hemi_d3d", "dotmap_hilo_hemi_gl", "dotmap_hilo_hemi",
    };
    int m = c->dotmap[stage] & 7;
    c->use_dotmap[m] = 1;
    switch (m) {
    case 1: c->use_snorm8[0] = 1; break;
    case 2: c->use_snorm8[1] = 1; break;
    case 3: c->use_snorm8[2] = 1; break;
    case 4: c->use_hilo16 = 1; break;
    case 5: c->use_hilo16 = c->use_hemi = c->use_snorm16[0] = 1; break;
    case 6: c->use_hilo16 = c->use_hemi = c->use_snorm16[1] = 1; break;
    case 7: c->use_hilo16 = c->use_hemi = c->use_snorm16[2] = 1; break;
    }
    return names[m];
}

/* "dotN" if stage N produced a dot product, else 0.0 */
static const char *dot_ref(psh_ctx *c, char *buf, size_t n, int stage)
{
    if (stage >= 0 && stage < 4 && c->dot_defined[stage]) snprintf(buf, n, "dot%d", stage);
    else snprintf(buf, n, "0.0");
    return buf;
}

static void emit_dot(psh_ctx *c, int i)
{
    sb_fmt(&c->tex, "    float dot%d = dot(gl_TexCoord[%d].xyz, %s(t%d));\n",
           i, i, dotmap_func(c, i), c->inputtex[i]);
    c->dot_defined[i] = 1;
}

/* Returns 0 when the mode cannot be expressed. */
static int emit_texture_stage(psh_ctx *c, int i)
{
    sbuf *o = &c->tex;
    int mode = c->texmode[i];
    int in = c->inputtex[i];
    char d1[16], d2[16];

    /* Modes that need previous stages or an input texture are only valid
       from stage 1, 2 or 3 on; the library rejects other defs, we just
       disable the stage. */
    if (mode != TM_NONE && mode != TM_PROJECT2D && mode != TM_PROJECT3D && mode != TM_CUBEMAP
        && mode != TM_PASSTHRU && mode != TM_CLIPPLANE) {
        int min_stage = 1;
        if (mode == TM_BRDF || mode == TM_DOT_ST || mode == TM_DOT_ZW || mode == TM_DOT_RFLCT_DIFF)
            min_stage = 2;
        if (mode == TM_DOT_RFLCT_SPEC || mode == TM_DOT_STR_3D || mode == TM_DOT_STR_CUBE
            || mode == TM_DOT_RFLCT_SPEC_CONST)
            min_stage = 3;
        if (i < min_stage) mode = TM_NONE;
        if (mode == TM_DOT_RFLCT_DIFF && i != 2) mode = TM_NONE;
        if (mode == TM_DOTPRODUCT && i == 3) mode = TM_NONE;
    }

    switch (mode) {
    case TM_NONE:
        sb_fmt(o, "    vec4 t%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
        break;
    case TM_PROJECT2D:
        c->sampler[i] = 2;
        c->use_texscale = 1;
        sb_fmt(o, "    vec4 t%d = texture2DProj(tex%d, vec3(gl_TexCoord[%d].xy * tex_scale[%d].xy, gl_TexCoord[%d].w));\n",
               i, i, i, i, i);
        break;
    case TM_PROJECT3D:
        c->sampler[i] = 3;
        c->use_texscale = 1;
        sb_fmt(o, "    vec4 t%d = texture3DProj(vol%d, vec4(gl_TexCoord[%d].xyz * tex_scale[%d].xyz, gl_TexCoord[%d].w));\n",
               i, i, i, i, i);
        break;
    case TM_CUBEMAP:
        c->sampler[i] = 6;
        sb_fmt(o, "    vec4 t%d = textureCube(cube%d, gl_TexCoord[%d].xyz);\n", i, i, i);
        break;
    case TM_PASSTHRU:
        sb_fmt(o, "    vec4 t%d = clamp(gl_TexCoord[%d], 0.0, 1.0);\n", i, i);
        break;
    case TM_CLIPPLANE:
        sb_fmt(o, "    vec4 t%d = vec4(0.0);\n", i);
        for (int j = 0; j < 4; j++)
            sb_fmt(o, "    if (gl_TexCoord[%d].%c %s 0.0) discard;\n", i, "xyzw"[j],
                   (c->compare[i] >> j) & 1 ? ">=" : "<");
        break;
    case TM_BUMPENVMAP:
    case TM_BUMPENVMAP_LUM:
        /* s' = s + m00*du + m10*dv, t' = t + m01*du + m11*dv (the DX8
           convention); du/dv are the signed bytes in the input's r/g. */
        c->sampler[i] = 2;
        c->use_texscale = c->use_bumpenv = c->use_snorm8[2] = 1;
        sb_fmt(o, "    vec2 dsdt%d = vec2(snorm8(t%d.r), snorm8(t%d.g));\n", i, in, in);
        sb_fmt(o, "    vec4 t%d = texture2D(tex%d, (gl_TexCoord[%d].xy + vec2(dot(bump_env[%d].xz, dsdt%d), dot(bump_env[%d].yw, dsdt%d))) * tex_scale[%d].xy);\n",
               i, i, i, i, i, i, i, i);
        if (mode == TM_BUMPENVMAP_LUM) {
            c->use_bumplum = 1;
            sb_fmt(o, "    t%d *= bump_lum[%d].x * t%d.b + bump_lum[%d].y;\n", i, i, in, i);
        }
        break;
    case TM_BRDF:
        /* The two previous stages hold the eye and light vectors in
           spherical coordinates as HILO (sigma, phi) pairs; look up the
           volume with (eyeSigma, lightSigma, eyePhi - lightPhi). */
        c->sampler[i] = 3;
        c->use_texscale = c->use_hilo16 = 1;
        sb_fmt(o, "    vec2 brdf_e%d = hilo16(t%d) / 65535.0;\n", i, i - 2);
        sb_fmt(o, "    vec2 brdf_l%d = hilo16(t%d) / 65535.0;\n", i, i - 1);
        sb_fmt(o, "    vec4 t%d = texture3D(vol%d, vec3(brdf_e%d.x, brdf_l%d.x, brdf_e%d.y - brdf_l%d.y) * tex_scale[%d].xyz);\n",
               i, i, i, i, i, i, i);
        break;
    case TM_DOT_ST:
        c->sampler[i] = 2;
        c->use_texscale = 1;
        emit_dot(c, i);
        sb_fmt(o, "    vec4 t%d = texture2D(tex%d, vec2(%s, dot%d) * tex_scale[%d].xy);\n",
               i, i, dot_ref(c, d1, sizeof d1, i - 1), i, i);
        break;
    case TM_DOT_ZW:
        emit_dot(c, i);
        c->use_frag_depth = 1;
        sb_fmt(o, "    vec4 t%d = vec4(0.0);\n", i);
        sb_fmt(o, "    float zw%d = %s / dot%d;\n", i, dot_ref(c, d1, sizeof d1, i - 1), i);
        sb_fmt(o, "    if (!(zw%d >= 0.0 && zw%d <= " ZW_DEPTH_SCALE ")) discard;\n", i, i);
        sb_fmt(o, "    gl_FragDepth = zw%d / " ZW_DEPTH_SCALE ";\n", i);
        break;
    case TM_DOT_RFLCT_DIFF:
        /* n = (dot[i-1], dot[i], dot computed with stage i+1's inputs) */
        c->sampler[i] = 6;
        emit_dot(c, i);
        sb_fmt(o, "    float dot%dn = dot(gl_TexCoord[%d].xyz, %s(t%d));\n",
               i, i + 1, dotmap_func(c, i + 1), c->inputtex[i + 1]);
        sb_fmt(o, "    vec4 t%d = textureCube(cube%d, vec3(%s, dot%d, dot%dn));\n",
               i, i, dot_ref(c, d1, sizeof d1, i - 1), i, i);
        break;
    case TM_DOT_RFLCT_SPEC:
        c->sampler[i] = 6;
        emit_dot(c, i);
        sb_fmt(o, "    vec3 n%d = vec3(%s, %s, dot%d);\n", i,
               dot_ref(c, d1, sizeof d1, i - 2), dot_ref(c, d2, sizeof d2, i - 1), i);
        sb_fmt(o, "    vec3 e%d = vec3(gl_TexCoord[%d].w, gl_TexCoord[%d].w, gl_TexCoord[%d].w);\n",
               i, i - 2, i - 1, i);
        sb_fmt(o, "    vec3 rv%d = 2.0 * n%d * dot(n%d, e%d) / dot(n%d, n%d) - e%d;\n",
               i, i, i, i, i, i, i);
        sb_fmt(o, "    vec4 t%d = textureCube(cube%d, rv%d);\n", i, i, i);
        break;
    case TM_DOT_STR_3D:
        c->sampler[i] = 3;
        c->use_texscale = 1;
        emit_dot(c, i);
        sb_fmt(o, "    vec4 t%d = texture3D(vol%d, vec3(%s, %s, dot%d) * tex_scale[%d].xyz);\n",
               i, i, dot_ref(c, d1, sizeof d1, i - 2), dot_ref(c, d2, sizeof d2, i - 1), i, i);
        break;
    case TM_DOT_STR_CUBE:
        c->sampler[i] = 6;
        emit_dot(c, i);
        sb_fmt(o, "    vec4 t%d = textureCube(cube%d, vec3(%s, %s, dot%d));\n",
               i, i, dot_ref(c, d1, sizeof d1, i - 2), dot_ref(c, d2, sizeof d2, i - 1), i);
        break;
    case TM_DPNDNT_AR:
        c->sampler[i] = 2;
        c->use_texscale = 1;
        sb_fmt(o, "    vec4 t%d = texture2D(tex%d, t%d.ar * tex_scale[%d].xy);\n", i, i, in, i);
        break;
    case TM_DPNDNT_GB:
        c->sampler[i] = 2;
        c->use_texscale = 1;
        sb_fmt(o, "    vec4 t%d = texture2D(tex%d, t%d.gb * tex_scale[%d].xy);\n", i, i, in, i);
        break;
    case TM_DOTPRODUCT:
        emit_dot(c, i);
        sb_fmt(o, "    vec4 t%d = vec4(0.0);\n", i);
        break;
    case TM_DOT_RFLCT_SPEC_CONST:
        /* The eye vector is the float value of D3D pixel shader constant
           0 (SetPixelShaderConstant pushes it to NV097_SET_EYE_VECTOR);
           nothing in the render state array or the uniform set carries
           it. */
        return 0;
    default:
        return 0;
    }
    return 1;
}

/* ---- combiner stages --------------------------------------------------- */

static void emit_half(psh_ctx *c, sbuf *calc, sbuf *store, int stage, uint32_t inputs, uint32_t outputs, int is_alpha)
{
    psh_input a, b, cc, d;
    psh_output out;
    char ea[128], eb[128], ec[128], ed[128], tmp[192];
    const char *T = is_alpha ? "float" : "vec3";
    const char *sfx = is_alpha ? "a" : "rgb";
    const char *wm = is_alpha ? ".a" : ".rgb";
    const char *dab, *dcd, *dsum;

    parse_inputs(inputs, &a, &b, &cc, &d);
    parse_output(&out, outputs);
    input_expr(c, ea, sizeof ea, a, is_alpha, 0, stage);
    input_expr(c, eb, sizeof eb, b, is_alpha, 0, stage);
    input_expr(c, ec, sizeof ec, cc, is_alpha, 0, stage);
    input_expr(c, ed, sizeof ed, d, is_alpha, 0, stage);

    sb_fmt(calc, "    %s A%s = %s, B%s = %s, C%s = %s, D%s = %s;\n", T, sfx, ea, sfx, eb, sfx, ec, sfx, ed);
    if (!is_alpha && out.ab_dot) sb_fmt(calc, "    vec3 AB = vec3(dot(Argb, Brgb));\n");
    else sb_fmt(calc, "    %s AB%s = A%s * B%s;\n", T, is_alpha ? "a" : "", sfx, sfx);
    if (!is_alpha && out.cd_dot) sb_fmt(calc, "    vec3 CD = vec3(dot(Crgb, Drgb));\n");
    else sb_fmt(calc, "    %s CD%s = C%s * D%s;\n", T, is_alpha ? "a" : "", sfx, sfx);
    if (!is_alpha) sfx = "";
    if (out.mux)
        sb_fmt(calc, "    %s SUM%s = mux ? CD%s : AB%s;\n", T, sfx, sfx, sfx);
    else
        sb_fmt(calc, "    %s SUM%s = AB%s + CD%s;\n", T, sfx, sfx, sfx);

    dab = dest_name(out.ab);
    dcd = dest_name(out.cd);
    dsum = dest_name(out.sum);
    /* The scaled, clamped outputs are computed before any register is
       written so that both halves see the stage's input values. */
    snprintf(tmp, sizeof tmp, "AB%s", sfx);
    output_expr(ea, sizeof ea, tmp, out.mapping);
    snprintf(tmp, sizeof tmp, "CD%s", sfx);
    output_expr(eb, sizeof eb, tmp, out.mapping);
    snprintf(tmp, sizeof tmp, "SUM%s", sfx);
    output_expr(ec, sizeof ec, tmp, out.mapping);
    if (dab) sb_fmt(calc, "    %s oAB%s = %s;\n", T, sfx, ea);
    if (dcd) sb_fmt(calc, "    %s oCD%s = %s;\n", T, sfx, eb);
    if (dsum) sb_fmt(calc, "    %s oSUM%s = %s;\n", T, sfx, ec);
    if (dab) sb_fmt(store, "    %s%s = oAB%s;\n", dab, wm, sfx);
    if (!is_alpha && out.ab_b2a && dab) sb_fmt(store, "    %s.a = oAB.b;\n", dab);
    if (dcd) sb_fmt(store, "    %s%s = oCD%s;\n", dcd, wm, sfx);
    if (!is_alpha && out.cd_b2a && dcd) sb_fmt(store, "    %s.a = oCD.b;\n", dcd);
    if (dsum) sb_fmt(store, "    %s%s = oSUM%s;\n", dsum, wm, sfx);
}

static void emit_stage(psh_ctx *c, int i)
{
    const uint32_t *rs = c->rs;
    sbuf *o = &c->comb;
    sbuf calc, store;
    psh_output orgb, oa;

    parse_output(&orgb, rs[RS_PSRGBOUTPUTS0 + i]);
    parse_output(&oa, rs[RS_PSALPHAOUTPUTS0 + i]);
    sb_fmt(o, "    /* stage %d */\n    {\n", i);
    if (orgb.mux || oa.mux) {
        if (c->flags & CC_MUX_MSB)
            sb_fmt(o, "    bool mux = r0.a >= 0.5;\n");
        else
            sb_fmt(o, "    bool mux = mod(floor(r0.a * 255.0 + 0.5), 2.0) >= 1.0;\n");
    }
    /* The RGB and alpha halves run in parallel on the hardware: both read
       the registers as they were before the stage, then the results are
       written. */
    sb_init(&calc);
    sb_init(&store);
    emit_half(c, &calc, &store, i, rs[RS_PSRGBINPUTS0 + i], rs[RS_PSRGBOUTPUTS0 + i], 0);
    emit_half(c, &calc, &store, i, rs[RS_PSALPHAINPUTS0 + i], rs[RS_PSALPHAOUTPUTS0 + i], 1);
    sb_cat(o, calc.s);
    sb_cat(o, store.s);
    sb_free(&calc);
    sb_free(&store);
    sb_fmt(o, "    }\n");
}

static void emit_final(psh_ctx *c, uint32_t abcd, uint32_t efg)
{
    sbuf *o = &c->comb;
    psh_input a, b, cc, d, e, f, g, unused;
    char ea[128], eb[128], ec[128], ed[128], ee[128], ef[128], eg[128];
    int settings = efg & 0xff;

    parse_inputs(abcd, &a, &b, &cc, &d);
    parse_inputs(efg, &e, &f, &g, &unused);
    input_expr(c, ee, sizeof ee, e, 0, 2, 8);
    input_expr(c, ef, sizeof ef, f, 0, 2, 8);
    input_expr(c, ea, sizeof ea, a, 0, 1, 8);
    input_expr(c, eb, sizeof eb, b, 0, 1, 8);
    input_expr(c, ec, sizeof ec, cc, 0, 1, 8);
    input_expr(c, ed, sizeof ed, d, 0, 1, 8);
    input_expr(c, eg, sizeof eg, g, 1, 2, 8);

    sb_fmt(o, "    /* final combiner */\n    {\n");
    if (c->use_v1r0) {
        char v1[48], r0[48];
        snprintf(v1, sizeof v1, "%s", (settings & FCS_COMPLEMENT_V1) ? "(1.0 - clamp(v1.rgb, 0.0, 1.0))" : "v1.rgb");
        snprintf(r0, sizeof r0, "%s", (settings & FCS_COMPLEMENT_R0) ? "(1.0 - clamp(r0.rgb, 0.0, 1.0))" : "r0.rgb");
        if (settings & FCS_CLAMP_SUM)
            sb_fmt(o, "    vec4 v1r0 = vec4(clamp(%s + %s, 0.0, 1.0), 0.0);\n", v1, r0);
        else
            sb_fmt(o, "    vec4 v1r0 = vec4(%s + %s, 0.0);\n", v1, r0);
    }
    if (c->use_efprod)
        sb_fmt(o, "    vec4 efprod = vec4(%s * %s, 0.0);\n", ee, ef);
    sb_fmt(o, "    vec3 fA = %s;\n    vec3 fB = %s;\n    vec3 fC = %s;\n    vec3 fD = %s;\n", ea, eb, ec, ed);
    sb_fmt(o, "    gl_FragColor = clamp(vec4(fD + mix(fC, fB, fA), %s), 0.0, 1.0);\n", eg);
    sb_fmt(o, "    }\n");
}

/* ---- top level --------------------------------------------------------- */

char *psh_translate(const uint32_t *rs)
{
    psh_ctx cx, *c = &cx;
    sbuf out;
    uint32_t abcd = rs[RS_PSFINALCOMBINERABCD], efg = rs[RS_PSFINALCOMBINEREFG];
    int fog_enable = rs[RS_FOGENABLE] != 0;
    int fog_mode = rs[RS_FOGTABLEMODE];

    memset(c, 0, sizeof *c);
    c->rs = rs;
    c->nstages = rs[RS_PSCOMBINERCOUNT] & 0xf;
    if (c->nstages > 8) c->nstages = 8;
    c->flags = rs[RS_PSCOMBINERCOUNT] >> 8;
    for (int i = 0; i < 4; i++) {
        c->texmode[i] = (rs[RS_PSTEXTUREMODES] >> (i * 5)) & 0x1f;
        c->compare[i] = (rs[RS_PSCOMPAREMODE] >> (i * 4)) & 0xf;
    }
    c->dotmap[1] = rs[RS_PSDOTMAPPING] & 7;
    c->dotmap[2] = (rs[RS_PSDOTMAPPING] >> 4) & 7;
    c->dotmap[3] = (rs[RS_PSDOTMAPPING] >> 8) & 7;
    c->inputtex[1] = 0;
    c->inputtex[2] = (rs[RS_PSINPUTTEXTURE] >> 16) & 1;
    c->inputtex[3] = (rs[RS_PSINPUTTEXTURE] >> 20) & 3;
    if (c->inputtex[3] > 2) c->inputtex[3] = 2;

    sb_init(&c->tex);
    sb_init(&c->comb);

    for (int i = 0; i < 4; i++) {
        if (!emit_texture_stage(c, i)) {
            sb_free(&c->tex);
            sb_free(&c->comb);
            return NULL;
        }
    }

    for (int i = 0; i < c->nstages; i++) emit_stage(c, i);

    if (abcd == 0 && efg == 0) {
        /* No xfc: the fixed-function final combiner the library leaves in
           place (LazySetSpecFogCombiner), minus specular. */
        if (fog_enable) {
            abcd = ((REG_FOG | CHAN_ALPHA) << 24) | (REG_R0 << 16) | (REG_FOG << 8) | REG_ZERO;
        } else {
            abcd = REG_R0;
        }
        efg = ((REG_ZERO) << 24) | (REG_ZERO << 16) | ((REG_R0 | CHAN_ALPHA) << 8) | FCS_CLAMP_SUM;
    }
    emit_final(c, abcd, efg);

    /* ---- assemble ---- */
    sb_init(&out);
    sb_cat(&out, "#version 120\n");
    for (int i = 0; i < 4; i++) {
        if (c->sampler[i] == 2) sb_fmt(&out, "uniform sampler2D tex%d;\n", i);
        if (c->sampler[i] == 3) sb_fmt(&out, "uniform sampler3D vol%d;\n", i);
        if (c->sampler[i] == 6) sb_fmt(&out, "uniform samplerCube cube%d;\n", i);
    }
    if (c->use_texscale) sb_cat(&out, "uniform vec4 tex_scale[4];\n");
    if (c->use_c0) sb_cat(&out, "uniform vec4 c0[8];\n");
    if (c->use_c1) sb_cat(&out, "uniform vec4 c1[8];\n");
    if (c->use_fc0) sb_cat(&out, "uniform vec4 fc0;\n");
    if (c->use_fc1) sb_cat(&out, "uniform vec4 fc1;\n");
    if (c->use_bumpenv) sb_cat(&out, "uniform vec4 bump_env[4];\n");
    if (c->use_bumplum) sb_cat(&out, "uniform vec2 bump_lum[4];\n");

    /* helpers: signed interpretations of unsigned-normalised texels */
    if (c->use_snorm8[0])
        sb_cat(&out, "float snorm8_d3d(float x) { return (x * 255.0 - 128.0) / 127.0; }\n");
    if (c->use_snorm8[1])
        sb_cat(&out, "float snorm8_gl(float x) { x *= 255.0; return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5; }\n");
    if (c->use_snorm8[2])
        sb_cat(&out, "float snorm8(float x) { x *= 255.0; return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }\n");
    if (c->use_hilo16)
        sb_cat(&out, "vec2 hilo16(vec4 c) { return vec2(c.a * 65280.0 + c.r * 255.0, c.g * 65280.0 + c.b * 255.0); }\n");
    if (c->use_snorm16[0])
        sb_cat(&out, "float snorm16_d3d(float x) { return x >= 32768.0 ? (x - 65536.0) / 32768.0 : x / 32768.0; }\n");
    if (c->use_snorm16[1])
        sb_cat(&out, "float snorm16_gl(float x) { return x >= 32768.0 ? (2.0 * (x - 65536.0) + 1.0) / 65535.0 : (2.0 * x + 1.0) / 65535.0; }\n");
    if (c->use_snorm16[2])
        sb_cat(&out, "float snorm16(float x) { return x >= 32768.0 ? (x - 65536.0) / 32767.0 : x / 32767.0; }\n");
    if (c->use_hemi)
        sb_cat(&out, "vec3 hemi(vec2 hl) { return vec3(hl, sqrt(max(0.0, 1.0 - dot(hl, hl)))); }\n");
    if (c->use_dotmap[0])
        sb_cat(&out, "vec3 dotmap_unsigned(vec4 c) { return c.rgb; }\n");
    if (c->use_dotmap[1])
        sb_cat(&out, "vec3 dotmap_snorm_d3d(vec4 c) { return vec3(snorm8_d3d(c.r), snorm8_d3d(c.g), snorm8_d3d(c.b)); }\n");
    if (c->use_dotmap[2])
        sb_cat(&out, "vec3 dotmap_snorm_gl(vec4 c) { return vec3(snorm8_gl(c.r), snorm8_gl(c.g), snorm8_gl(c.b)); }\n");
    if (c->use_dotmap[3])
        sb_cat(&out, "vec3 dotmap_snorm(vec4 c) { return vec3(snorm8(c.r), snorm8(c.g), snorm8(c.b)); }\n");
    if (c->use_dotmap[4])
        sb_cat(&out, "vec3 dotmap_hilo_1(vec4 c) { return vec3(hilo16(c) / 65535.0, 1.0); }\n");
    if (c->use_dotmap[5])
        sb_cat(&out, "vec3 dotmap_hilo_hemi_d3d(vec4 c) { vec2 h = hilo16(c); return hemi(vec2(snorm16_d3d(h.x), snorm16_d3d(h.y))); }\n");
    if (c->use_dotmap[6])
        sb_cat(&out, "vec3 dotmap_hilo_hemi_gl(vec4 c) { vec2 h = hilo16(c); return hemi(vec2(snorm16_gl(h.x), snorm16_gl(h.y))); }\n");
    if (c->use_dotmap[7])
        sb_cat(&out, "vec3 dotmap_hilo_hemi(vec4 c) { vec2 h = hilo16(c); return hemi(vec2(snorm16(h.x), snorm16(h.y))); }\n");

    sb_cat(&out, "void main()\n{\n");
    sb_cat(&out, "    vec4 v0 = gl_Color;\n    vec4 v1 = gl_SecondaryColor;\n");
    if (c->use_fog) {
        const char *f;
        if (!fog_enable) f = "1.0";
        else if (fog_mode == 3) f = "clamp((gl_Fog.end - gl_FogFragCoord) * gl_Fog.scale, 0.0, 1.0)";
        else if (fog_mode == 1) f = "clamp(exp(-gl_Fog.density * gl_FogFragCoord), 0.0, 1.0)";
        else if (fog_mode == 2) f = "clamp(exp(-(gl_Fog.density * gl_FogFragCoord) * (gl_Fog.density * gl_FogFragCoord)), 0.0, 1.0)";
        else f = "clamp(gl_FogFragCoord, 0.0, 1.0)";
        sb_fmt(&out, "    vec4 fog = vec4(gl_Fog.color.rgb, %s);\n", f);
    }
    sb_cat(&out, c->tex.s);
    sb_cat(&out, "    vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a);\n    vec4 r1 = vec4(0.0);\n");
    sb_cat(&out, c->comb.s);
    sb_cat(&out, "}\n");

    sb_free(&c->tex);
    sb_free(&c->comb);
    return out.s;
}
