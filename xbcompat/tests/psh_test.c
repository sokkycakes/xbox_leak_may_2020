/*
 * psh_test: feed pixel shader defs to psh_translate() and compile the GLSL
 * it produces with the real driver (build/glslcheck).
 *
 *   psh_test <samples dir> <glslcheck> [outdir]
 *
 * <samples dir> is ".../xbox trunk/xbox/private/atg/samples"; every .xpu
 * the samples ship is translated, plus hand-built defs covering each
 * texture mode, dot mapping, combiner feature and final combiner special.
 * The GLSL goes to <outdir> (default /tmp/claude-0/xbe/psh) as
 * <name>.frag, with the driver log beside it as <name>.log.  Needs a
 * display for glslcheck: run under xvfb-run.
 *
 * Build:  gcc -std=gnu11 -Wall -I src/hle tests/psh_test.c src/hle/psh.c -o psh_test
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "nv2a_shaders.h"

/* ---- the SDK macros (d3d8types.h) ---- */
#define PS_TEXTUREMODES(t0, t1, t2, t3) (((t3) << 15) | ((t2) << 10) | ((t1) << 5) | (t0))
#define PS_DOTMAPPING(t0, t1, t2, t3) (((t3) << 8) | ((t2) << 4) | (t1))
#define PS_COMPAREMODE(t0, t1, t2, t3) (((t3) << 12) | ((t2) << 8) | ((t1) << 4) | (t0))
#define PS_INPUTTEXTURE(t0, t1, t2, t3) (((t3) << 20) | ((t2) << 16))
#define PS_COMBINERCOUNT(count, flags) (((flags) << 8) | (count))
#define PS_COMBINERINPUTS(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define PS_COMBINEROUTPUTS(ab, cd, mux_sum, flags) (((flags) << 12) | ((mux_sum) << 8) | ((ab) << 4) | (cd))

enum {
    TM_NONE, TM_PROJECT2D, TM_PROJECT3D, TM_CUBEMAP, TM_PASSTHRU, TM_CLIPPLANE, TM_BUMPENVMAP,
    TM_BUMPENVMAP_LUM, TM_BRDF, TM_DOT_ST, TM_DOT_ZW, TM_DOT_RFLCT_DIFF, TM_DOT_RFLCT_SPEC,
    TM_DOT_STR_3D, TM_DOT_STR_CUBE, TM_DPNDNT_AR, TM_DPNDNT_GB, TM_DOTPRODUCT, TM_DOT_RFLCT_SPEC_CONST,
};
enum {
    ZERO = 0, C0 = 1, C1 = 2, FOG = 3, V0 = 4, V1 = 5, T0 = 8, T1 = 9, T2 = 10, T3 = 11,
    R0 = 12, R1 = 13, V1R0_SUM = 14, EF_PROD = 15, DISCARD = 0,
};
enum {
    UNSIGNED_IDENTITY = 0x00, UNSIGNED_INVERT = 0x20, EXPAND_NORMAL = 0x40, EXPAND_NEGATE = 0x60,
    HALFBIAS_NORMAL = 0x80, HALFBIAS_NEGATE = 0xa0, SIGNED_IDENTITY = 0xc0, SIGNED_NEGATE = 0xe0,
};
enum { RGB = 0, BLUE = 0, ALPHA = 0x10 };
enum {
    IDENTITY = 0, BIAS = 0x08, SHIFTLEFT_1 = 0x10, SHIFTLEFT_1_BIAS = 0x18, SHIFTLEFT_2 = 0x20,
    SHIFTRIGHT_1 = 0x30, AB_BLUE_TO_ALPHA = 0x80, CD_BLUE_TO_ALPHA = 0x40, AB_DOT_PRODUCT = 0x02,
    CD_DOT_PRODUCT = 0x01, AB_CD_MUX = 0x04,
};
enum { MUX_MSB = 1, UNIQUE_C0 = 0x10, UNIQUE_C1 = 0x100 };
enum { CLAMP_SUM = 0x80, COMPLEMENT_V1 = 0x40, COMPLEMENT_R0 = 0x20 };
#define ONE (ZERO | UNSIGNED_INVERT)

/* def dword indices */
enum {
    D_ALPHAIN = 0, D_FCABCD = 8, D_FCEFG = 9, D_CONST0 = 10, D_CONST1 = 18, D_ALPHAOUT = 26,
    D_RGBIN = 34, D_COMPARE = 42, D_FCCONST0 = 43, D_FCCONST1 = 44, D_RGBOUT = 45,
    D_COUNT = 53, D_TEXMODES = 54, D_DOTMAP = 55, D_INPUTTEX = 56, D_C0MAP = 57, D_C1MAP = 58,
    D_FCCONSTS = 59,
};

static const char *outdir = "/tmp/claude-0/xbe/psh";
static const char *glslcheck;
static int failures, passes;

static void rs_from_def(uint32_t *rs, const uint32_t *def, int fog_enable, int fog_mode)
{
    memset(rs, 0, 128 * sizeof *rs);
    memcpy(rs, def, 57 * sizeof *rs);
    rs[117] = def[D_TEXMODES];
    rs[82] = fog_enable;
    rs[83] = fog_mode;
}

static int run_glslcheck(const char *what, const char *a, const char *b, const char *log)
{
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "\"%s\" %s \"%s\" %s%s%s > \"%s\" 2>&1", glslcheck, what, a,
             b ? "\"" : "", b ? b : "", b ? "\"" : "", log);
    return system(cmd) == 0;
}

/* Translate, write, compile.  expect_null: the translator must refuse. */
static char *check(const char *name, const uint32_t *rs, int expect_null)
{
    char path[512], log[512];
    char *src = psh_translate(rs);

    if (expect_null) {
        if (src) {
            printf("FAIL %-28s expected NULL, got a shader\n", name);
            failures++;
            free(src);
        } else {
            printf("ok   %-28s refused as expected\n", name);
            passes++;
        }
        return NULL;
    }
    if (!src) {
        printf("FAIL %-28s translator returned NULL\n", name);
        failures++;
        return NULL;
    }
    snprintf(path, sizeof path, "%s/%s.frag", outdir, name);
    snprintf(log, sizeof log, "%s/%s.log", outdir, name);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(src, f);
    fclose(f);
    if (run_glslcheck("frag", path, NULL, log)) {
        printf("ok   %-28s %s\n", name, path);
        passes++;
    } else {
        printf("FAIL %-28s %s (see %s)\n", name, path, log);
        failures++;
    }
    return src;
}

static int has(const char *src, const char *needle)
{
    return src && strstr(src, needle) != NULL;
}

static void expect(const char *name, const char *src, const char *needle, int present)
{
    if (!src) return;
    if (has(src, needle) != present) {
        printf("FAIL %-28s %s \"%s\"\n", name, present ? "missing" : "unexpected", needle);
        failures++;
    }
}

/* ---- .xpu files ---- */
static const char *xpus[] = {
    "graphics/AntiAlias/media/Shaders/carTransparent.xpu",
    "graphics/AntiAlias/media/Shaders/caropaque.xpu",
    "graphics/BRDF/media/shaders/brdf.xpu",
    "graphics/DolphinClassic/media/shaders/dolphin.xpu",
    "graphics/Fire/media/shaders/Fire.xpu",
    "graphics/PTM/Media/Shaders/PTM.xpu",
    "graphics/PersistDisplay/media/shaders/dolphin.xpu",
    "graphics/PixelShader/media/shaders/pshader.xpu",
    "graphics/PolynomialTextureMaps/Media/Shaders/PTM.xpu",
    "graphics/TextureCompression/media/shaders/comp.xpu",
    "graphics/shadowbuffer/media/shaders/shadwbuf.xpu",
};

static void test_xpus(const char *root)
{
    for (unsigned i = 0; i < sizeof xpus / sizeof *xpus; i++) {
        char path[1024], name[128];
        uint32_t file[61], rs[128];
        snprintf(path, sizeof path, "%s/%s", root, xpus[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { perror(path); failures++; continue; }
        size_t n = fread(file, 4, 61, f);
        fclose(f);
        if (n != 61 || file[0] != 0x30425350) {
            printf("FAIL %s: not a PSB0 file (%zu dwords, id %08x)\n", path, n, file[0]);
            failures++;
            continue;
        }
        /* name: <sample>_<file> */
        const char *s = strstr(xpus[i], "graphics/") + 9;
        const char *slash = strchr(s, '/');
        const char *base = strrchr(xpus[i], '/') + 1;
        snprintf(name, sizeof name, "%.*s_%.*s", (int)(slash - s), s, (int)(strlen(base) - 4), base);
        rs_from_def(rs, file + 1, 0, 0);
        char *src = check(name, rs, 0);
        free(src);
    }
}

/* ---- hand-built defs ---- */

/* A def whose single stage computes r0 = T * v0 (rgb) / r0.a = T.a, with
   no xfc (fixed-function final combiner). */
static void simple_def(uint32_t *def, int treg, uint32_t modes, uint32_t dotmap, uint32_t inputtex)
{
    memset(def, 0, 60 * sizeof *def);
    def[D_COUNT] = PS_COMBINERCOUNT(1, 0);
    def[D_TEXMODES] = modes;
    def[D_DOTMAP] = dotmap;
    def[D_INPUTTEX] = inputtex;
    def[D_RGBIN] = PS_COMBINERINPUTS(treg | RGB, V0 | RGB, ZERO, ZERO);
    def[D_ALPHAIN] = PS_COMBINERINPUTS(treg | ALPHA, ONE | ALPHA, ZERO, ZERO);
    def[D_RGBOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);
    def[D_ALPHAOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);
}

static void test_texture_modes(void)
{
    uint32_t def[60], rs[128];
    char *src;

    /* basic modes at every stage */
    simple_def(def, T3, PS_TEXTUREMODES(TM_NONE, TM_PROJECT2D, TM_PROJECT3D, TM_CUBEMAP), 0, 0);
    rs_from_def(rs, def, 0, 0);
    src = check("tm_basic", rs, 0);
    expect("tm_basic", src, "uniform sampler2D tex1;", 1);
    expect("tm_basic", src, "uniform sampler3D vol2;", 1);
    expect("tm_basic", src, "uniform samplerCube cube3;", 1);
    expect("tm_basic", src, "tex0", 0);
    expect("tm_basic", src, "texture2DProj(tex1", 1);
    expect("tm_basic", src, "texture3DProj(vol2", 1);
    expect("tm_basic", src, "textureCube(cube3, gl_TexCoord[3].xyz)", 1);
    free(src);

    simple_def(def, T0, PS_TEXTUREMODES(TM_PASSTHRU, TM_CLIPPLANE, TM_NONE, TM_NONE), 0, 0);
    def[D_COMPARE] = PS_COMPAREMODE(0, 1 | 4, 0, 0); /* s >= , t <, r >=, q < */
    rs_from_def(rs, def, 0, 0);
    src = check("tm_passthru_clip", rs, 0);
    expect("tm_passthru_clip", src, "vec4 t0 = clamp(gl_TexCoord[0], 0.0, 1.0);", 1);
    expect("tm_passthru_clip", src, "if (gl_TexCoord[1].x >= 0.0) discard;", 1);
    expect("tm_passthru_clip", src, "if (gl_TexCoord[1].y < 0.0) discard;", 1);
    expect("tm_passthru_clip", src, "if (gl_TexCoord[1].z >= 0.0) discard;", 1);
    expect("tm_passthru_clip", src, "if (gl_TexCoord[1].w < 0.0) discard;", 1);
    expect("tm_passthru_clip", src, "uniform", 0);
    free(src);

    /* bump env: stage 1 perturbed by t0, stage 2 (luminance) by t1 */
    simple_def(def, T2, PS_TEXTUREMODES(TM_PROJECT2D, TM_BUMPENVMAP, TM_BUMPENVMAP_LUM, TM_NONE), 0,
               PS_INPUTTEXTURE(0, 0, 1, 0));
    rs_from_def(rs, def, 0, 0);
    src = check("tm_bumpenv", rs, 0);
    expect("tm_bumpenv", src, "uniform vec4 bump_env[4];", 1);
    expect("tm_bumpenv", src, "uniform vec2 bump_lum[4];", 1);
    expect("tm_bumpenv", src, "vec2 dsdt1 = vec2(snorm8(t0.r), snorm8(t0.g));", 1);
    expect("tm_bumpenv", src, "vec2 dsdt2 = vec2(snorm8(t1.r), snorm8(t1.g));", 1);
    expect("tm_bumpenv", src, "t2 *= bump_lum[2].x * t1.b + bump_lum[2].y;", 1);
    free(src);

    /* BRDF as the BRDF sample sets it up */
    simple_def(def, T3, PS_TEXTUREMODES(TM_PROJECT2D, TM_CUBEMAP, TM_CUBEMAP, TM_BRDF),
               PS_DOTMAPPING(0, 1, 1, 1), 0);
    rs_from_def(rs, def, 0, 0);
    src = check("tm_brdf", rs, 0);
    expect("tm_brdf", src, "uniform sampler3D vol3;", 1);
    expect("tm_brdf", src, "hilo16(t1)", 1);
    expect("tm_brdf", src, "hilo16(t2)", 1);
    free(src);

    /* DOT_ST (Minnaert style): t2 = tex(t1.t0, t2.t0) */
    simple_def(def, T2, PS_TEXTUREMODES(TM_CUBEMAP, TM_DOTPRODUCT, TM_DOT_ST, TM_NONE),
               PS_DOTMAPPING(0, 1, 1, 0), PS_INPUTTEXTURE(0, 0, 0, 0));
    rs_from_def(rs, def, 0, 0);
    src = check("tm_dot_st", rs, 0);
    expect("tm_dot_st", src, "float dot1 = dot(gl_TexCoord[1].xyz, dotmap_snorm_d3d(t0));", 1);
    expect("tm_dot_st", src, "float dot2 = dot(gl_TexCoord[2].xyz, dotmap_snorm_d3d(t0));", 1);
    expect("tm_dot_st", src, "texture2D(tex2, vec2(dot1, dot2) * tex_scale[2].xy)", 1);
    free(src);

    /* DOT_ZW as in ZSprite: HILO_1 on a depth texture */
    simple_def(def, T0, PS_TEXTUREMODES(TM_PROJECT2D, TM_PROJECT2D, TM_DOTPRODUCT, TM_DOT_ZW),
               PS_DOTMAPPING(0, 0, 4, 4), PS_INPUTTEXTURE(0, 0, 1, 1));
    rs_from_def(rs, def, 0, 0);
    src = check("tm_dot_zw", rs, 0);
    expect("tm_dot_zw", src, "dotmap_hilo_1(t1)", 1);
    expect("tm_dot_zw", src, "gl_FragDepth = zw3 / 16777215.0;", 1);
    expect("tm_dot_zw", src, "float zw3 = dot2 / dot3;", 1);
    free(src);

    /* DOT_RFLCT_DIFF + DOT_RFLCT_SPEC (BumpDemo) */
    simple_def(def, T3, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOT_RFLCT_DIFF, TM_DOT_RFLCT_SPEC),
               PS_DOTMAPPING(0, 3, 3, 3), 0);
    def[D_RGBIN] = PS_COMBINERINPUTS(T3 | RGB, V0 | RGB, T2 | RGB, ONE | RGB);
    def[D_RGBOUT] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, R0, IDENTITY);
    rs_from_def(rs, def, 0, 0);
    src = check("tm_dot_reflect", rs, 0);
    expect("tm_dot_reflect", src, "uniform samplerCube cube2;", 1);
    expect("tm_dot_reflect", src, "uniform samplerCube cube3;", 1);
    expect("tm_dot_reflect", src, "float dot2n = dot(gl_TexCoord[3].xyz, dotmap_snorm(t0));", 1);
    expect("tm_dot_reflect", src, "textureCube(cube2, vec3(dot1, dot2, dot2n))", 1);
    expect("tm_dot_reflect", src, "vec3 n3 = vec3(dot1, dot2, dot3);", 1);
    expect("tm_dot_reflect", src, "vec3 e3 = vec3(gl_TexCoord[1].w, gl_TexCoord[2].w, gl_TexCoord[3].w);", 1);
    expect("tm_dot_reflect", src, "textureCube(cube3, rv3)", 1);
    free(src);

    /* DOT_STR_3D / DOT_STR_CUBE */
    simple_def(def, T3, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOTPRODUCT, TM_DOT_STR_3D),
               PS_DOTMAPPING(0, 2, 2, 2), PS_INPUTTEXTURE(0, 0, 0, 0));
    rs_from_def(rs, def, 0, 0);
    src = check("tm_dot_str_3d", rs, 0);
    expect("tm_dot_str_3d", src, "texture3D(vol3, vec3(dot1, dot2, dot3) * tex_scale[3].xyz)", 1);
    expect("tm_dot_str_3d", src, "dotmap_snorm_gl(t0)", 1);
    free(src);

    simple_def(def, T3, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOTPRODUCT, TM_DOT_STR_CUBE),
               PS_DOTMAPPING(0, 0, 0, 0), 0);
    rs_from_def(rs, def, 0, 0);
    src = check("tm_dot_str_cube", rs, 0);
    expect("tm_dot_str_cube", src, "textureCube(cube3, vec3(dot1, dot2, dot3))", 1);
    expect("tm_dot_str_cube", src, "dotmap_unsigned(t0)", 1);
    free(src);

    /* dependent AR / GB with the input texture selectors */
    simple_def(def, T3, PS_TEXTUREMODES(TM_PROJECT2D, TM_DPNDNT_AR, TM_PROJECT2D, TM_DPNDNT_GB), 0,
               PS_INPUTTEXTURE(0, 0, 0, 2));
    rs_from_def(rs, def, 0, 0);
    src = check("tm_dependent", rs, 0);
    expect("tm_dependent", src, "texture2D(tex1, t0.ar * tex_scale[1].xy)", 1);
    expect("tm_dependent", src, "texture2D(tex3, t2.gb * tex_scale[3].xy)", 1);
    free(src);

    /* the HILO hemisphere mappings */
    for (int m = 5; m <= 7; m++) {
        char name[32];
        snprintf(name, sizeof name, "tm_dotmap%d", m);
        simple_def(def, T2, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOT_ST, TM_NONE),
                   PS_DOTMAPPING(0, m, m, 0), 0);
        rs_from_def(rs, def, 0, 0);
        src = check(name, rs, 0);
        expect(name, src, "hemi(", 1);
        free(src);
    }

    /* constant-eye reflection cannot be expressed */
    simple_def(def, T3, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOTPRODUCT, TM_DOT_RFLCT_SPEC_CONST),
               0, 0);
    rs_from_def(rs, def, 0, 0);
    check("tm_spec_const", rs, 1);

    /* a mode in a stage it is not valid for just disables the stage */
    simple_def(def, T0, PS_TEXTUREMODES(TM_BUMPENVMAP, TM_NONE, TM_NONE, TM_NONE), 0, 0);
    rs_from_def(rs, def, 0, 0);
    src = check("tm_invalid_stage", rs, 0);
    expect("tm_invalid_stage", src, "vec4 t0 = vec4(0.0, 0.0, 0.0, 1.0);", 1);
    free(src);
}

static void test_combiners(void)
{
    uint32_t def[60], rs[128];
    char *src;

    /* every input mapping and channel, on all 8 stages, unique constants */
    memset(def, 0, sizeof def);
    def[D_COUNT] = PS_COMBINERCOUNT(8, UNIQUE_C0 | UNIQUE_C1);
    def[D_TEXMODES] = PS_TEXTUREMODES(TM_PROJECT2D, TM_PROJECT2D, TM_NONE, TM_NONE);
    for (int i = 0; i < 8; i++) {
        int map = i << 5;
        def[D_RGBIN + i] = PS_COMBINERINPUTS(T0 | RGB | map, C0 | ALPHA | map, V1 | RGB | map, C1 | RGB | map);
        def[D_ALPHAIN + i] = PS_COMBINERINPUTS(T1 | BLUE | map, C1 | ALPHA | map, FOG | ALPHA | map, R0 | BLUE | map);
        def[D_RGBOUT + i] = PS_COMBINEROUTPUTS(R0, R1, DISCARD, IDENTITY);
        def[D_ALPHAOUT + i] = PS_COMBINEROUTPUTS(R0, R1, DISCARD, IDENTITY);
    }
    rs_from_def(rs, def, 0, 0);
    src = check("cb_inputs", rs, 0);
    expect("cb_inputs", src, "uniform vec4 c0[8];", 1);
    expect("cb_inputs", src, "uniform vec4 c1[8];", 1);
    expect("cb_inputs", src, "c0[7].aaa", 1);
    expect("cb_inputs", src, "c1[7].a", 1);
    expect("cb_inputs", src, "vec4 fog = vec4(gl_Fog.color.rgb, 1.0);", 1);
    expect("cb_inputs", src, "max(t0.rgb, 0.0)", 1);
    expect("cb_inputs", src, "(1.0 - clamp(t0.rgb, 0.0, 1.0))", 1);
    expect("cb_inputs", src, "(2.0 * max(t0.rgb, 0.0) - 1.0)", 1);
    expect("cb_inputs", src, "(1.0 - 2.0 * max(t0.rgb, 0.0))", 1);
    expect("cb_inputs", src, "(max(t0.rgb, 0.0) - 0.5)", 1);
    expect("cb_inputs", src, "(0.5 - max(t0.rgb, 0.0))", 1);
    expect("cb_inputs", src, "= t0.rgb,", 1);
    expect("cb_inputs", src, "(-t0.rgb)", 1);
    expect("cb_inputs", src, "r0.b", 1);
    free(src);

    /* shared constants use c0[0]/c1[0] in every stage */
    def[D_COUNT] = PS_COMBINERCOUNT(8, 0);
    rs_from_def(rs, def, 0, 0);
    src = check("cb_shared_consts", rs, 0);
    expect("cb_shared_consts", src, "c0[0]", 1);
    expect("cb_shared_consts", src, "c0[7]", 0);
    expect("cb_shared_consts", src, "c1[1]", 0);
    free(src);

    /* every output mapping, blue to alpha, dot products, sum */
    memset(def, 0, sizeof def);
    def[D_COUNT] = PS_COMBINERCOUNT(6, 0);
    def[D_TEXMODES] = PS_TEXTUREMODES(TM_PROJECT2D, TM_PROJECT2D, TM_NONE, TM_NONE);
    {
        static const int maps[6] = { IDENTITY, BIAS, SHIFTLEFT_1, SHIFTLEFT_1_BIAS, SHIFTLEFT_2, SHIFTRIGHT_1 };
        for (int i = 0; i < 6; i++) {
            def[D_RGBIN + i] = PS_COMBINERINPUTS(T0 | RGB | EXPAND_NORMAL, T1 | RGB | EXPAND_NORMAL,
                                                  V0 | RGB, V1 | RGB);
            def[D_ALPHAIN + i] = PS_COMBINERINPUTS(T0 | ALPHA, T1 | ALPHA, V0 | ALPHA, V1 | ALPHA);
            def[D_RGBOUT + i] = PS_COMBINEROUTPUTS(R0, R1, DISCARD, maps[i] | AB_DOT_PRODUCT | CD_DOT_PRODUCT | AB_BLUE_TO_ALPHA | CD_BLUE_TO_ALPHA);
            def[D_ALPHAOUT + i] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, R0, maps[i]);
        }
    }
    rs_from_def(rs, def, 0, 0);
    src = check("cb_outputs", rs, 0);
    expect("cb_outputs", src, "vec3 AB = vec3(dot(Argb, Brgb));", 1);
    expect("cb_outputs", src, "vec3 CD = vec3(dot(Crgb, Drgb));", 1);
    expect("cb_outputs", src, "r0.a = oAB.b;", 1);
    expect("cb_outputs", src, "r1.a = oCD.b;", 1);
    expect("cb_outputs", src, "clamp(AB - 0.5, -1.0, 1.0)", 1);
    expect("cb_outputs", src, "clamp(AB * 2.0, -1.0, 1.0)", 1);
    expect("cb_outputs", src, "clamp((AB - 0.5) * 2.0, -1.0, 1.0)", 1);
    expect("cb_outputs", src, "clamp(AB * 4.0, -1.0, 1.0)", 1);
    expect("cb_outputs", src, "clamp(AB * 0.5, -1.0, 1.0)", 1);
    expect("cb_outputs", src, "clamp(SUMa * 0.5, -1.0, 1.0)", 1);
    expect("cb_outputs", src, "r0.a = oSUMa;", 1);
    free(src);

    /* mux, both flavours; writes to v0 and t0 */
    memset(def, 0, sizeof def);
    def[D_COUNT] = PS_COMBINERCOUNT(2, MUX_MSB);
    def[D_TEXMODES] = PS_TEXTUREMODES(TM_PROJECT2D, TM_PROJECT2D, TM_PROJECT2D, TM_NONE);
    def[D_RGBIN] = PS_COMBINERINPUTS(T2 | RGB, ONE | RGB, ZERO, ZERO);
    def[D_ALPHAIN] = PS_COMBINERINPUTS(T2 | BLUE, ONE | ALPHA, ZERO, ZERO);
    def[D_RGBOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);
    def[D_ALPHAOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);
    def[D_RGBIN + 1] = PS_COMBINERINPUTS(T0 | RGB, ONE | RGB, T1 | RGB, ONE | RGB);
    def[D_ALPHAIN + 1] = PS_COMBINERINPUTS(T0 | ALPHA, ONE | ALPHA, T1 | ALPHA, ONE | ALPHA);
    def[D_RGBOUT + 1] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, V0, AB_CD_MUX);
    def[D_ALPHAOUT + 1] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, T0, AB_CD_MUX);
    def[D_FCABCD] = PS_COMBINERINPUTS(ZERO, ZERO, ZERO, V0 | RGB);
    def[D_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, T0 | ALPHA, 0);
    rs_from_def(rs, def, 0, 0);
    src = check("cb_mux_msb", rs, 0);
    expect("cb_mux_msb", src, "bool mux = r0.a >= 0.5;", 1);
    expect("cb_mux_msb", src, "vec3 SUM = mux ? CD : AB;", 1);
    expect("cb_mux_msb", src, "float SUMa = mux ? CDa : ABa;", 1);
    expect("cb_mux_msb", src, "v0.rgb = oSUM;", 1);
    expect("cb_mux_msb", src, "t0.a = oSUMa;", 1);
    free(src);
    def[D_COUNT] = PS_COMBINERCOUNT(2, 0);
    rs_from_def(rs, def, 0, 0);
    src = check("cb_mux_lsb", rs, 0);
    expect("cb_mux_lsb", src, "bool mux = mod(floor(r0.a * 255.0 + 0.5), 2.0) >= 1.0;", 1);
    free(src);

    /* zero stages: only the final combiner, r0.a = t0.a */
    memset(def, 0, sizeof def);
    def[D_TEXMODES] = PS_TEXTUREMODES(TM_PROJECT2D, TM_NONE, TM_NONE, TM_NONE);
    rs_from_def(rs, def, 0, 0);
    src = check("cb_no_stages", rs, 0);
    expect("cb_no_stages", src, "/* stage 0 */", 0);
    expect("cb_no_stages", src, "vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a);", 1);
    free(src);
}

static void test_final_combiner(void)
{
    uint32_t def[60], rs[128];
    char *src;

    /* xfc with the sum and product specials and all settings */
    simple_def(def, T0, PS_TEXTUREMODES(TM_PROJECT2D, TM_NONE, TM_NONE, TM_NONE), 0, 0);
    def[D_FCABCD] = PS_COMBINERINPUTS(T0 | ALPHA, V1R0_SUM | RGB, EF_PROD | RGB | UNSIGNED_INVERT, C0 | RGB);
    def[D_FCEFG] = PS_COMBINERINPUTS(R0 | RGB, C1 | RGB, R1 | BLUE, CLAMP_SUM | COMPLEMENT_V1 | COMPLEMENT_R0);
    rs_from_def(rs, def, 0, 0);
    src = check("fc_specials", rs, 0);
    expect("fc_specials", src, "uniform vec4 fc0;", 1);
    expect("fc_specials", src, "uniform vec4 fc1;", 1);
    expect("fc_specials", src, "vec4 v1r0 = vec4(clamp((1.0 - clamp(v1.rgb, 0.0, 1.0)) + (1.0 - clamp(r0.rgb, 0.0, 1.0)), 0.0, 1.0), 0.0);", 1);
    expect("fc_specials", src, "vec4 efprod = vec4(max(r0.rgb, 0.0) * max(fc1.rgb, 0.0), 0.0);", 1);
    expect("fc_specials", src, "vec3 fA = max(t0.aaa, 0.0);", 1);
    expect("fc_specials", src, "vec3 fB = max(v1r0.rgb, 0.0);", 1);
    expect("fc_specials", src, "vec3 fC = (1.0 - clamp(efprod.rgb, 0.0, 1.0));", 1);
    expect("fc_specials", src, "vec3 fD = max(fc0.rgb, 0.0);", 1);
    expect("fc_specials", src, "gl_FragColor = clamp(vec4(fD + mix(fC, fB, fA), max(r1.b, 0.0)), 0.0, 1.0);", 1);
    free(src);

    /* unclamped sum, no complements */
    def[D_FCEFG] = PS_COMBINERINPUTS(R0 | RGB, C1 | RGB, R1 | ALPHA, 0);
    rs_from_def(rs, def, 0, 0);
    src = check("fc_sum_unclamped", rs, 0);
    expect("fc_sum_unclamped", src, "vec4 v1r0 = vec4(v1.rgb + r0.rgb, 0.0);", 1);
    free(src);

    /* no xfc, fog off: out = r0 */
    simple_def(def, T0, PS_TEXTUREMODES(TM_PROJECT2D, TM_NONE, TM_NONE, TM_NONE), 0, 0);
    rs_from_def(rs, def, 0, 0);
    src = check("fc_default", rs, 0);
    expect("fc_default", src, "vec3 fD = max(r0.rgb, 0.0);", 1);
    expect("fc_default", src, "gl_FragColor = clamp(vec4(fD + mix(fC, fB, fA), max(r0.a, 0.0)), 0.0, 1.0);", 1);
    expect("fc_default", src, "gl_Fog", 0);
    free(src);

    /* no xfc, fog on, each table mode */
    static const char *fogexpr[4] = {
        "vec4 fog = vec4(gl_Fog.color.rgb, clamp(gl_FogFragCoord, 0.0, 1.0));",
        "vec4 fog = vec4(gl_Fog.color.rgb, clamp(exp(-gl_Fog.density * gl_FogFragCoord), 0.0, 1.0));",
        "vec4 fog = vec4(gl_Fog.color.rgb, clamp(exp(-(gl_Fog.density * gl_FogFragCoord) * (gl_Fog.density * gl_FogFragCoord)), 0.0, 1.0));",
        "vec4 fog = vec4(gl_Fog.color.rgb, clamp((gl_Fog.end - gl_FogFragCoord) * gl_Fog.scale, 0.0, 1.0));",
    };
    for (int mode = 0; mode < 4; mode++) {
        char name[32];
        snprintf(name, sizeof name, "fc_fog%d", mode);
        rs_from_def(rs, def, 1, mode);
        src = check(name, rs, 0);
        expect(name, src, fogexpr[mode], 1);
        expect(name, src, "vec3 fA = max(fog.aaa, 0.0);", 1);
        expect(name, src, "vec3 fB = max(r0.rgb, 0.0);", 1);
        expect(name, src, "vec3 fC = max(fog.rgb, 0.0);", 1);
        free(src);
    }

    /* xfc that reads the fog register itself, fog on */
    def[D_FCABCD] = PS_COMBINERINPUTS(FOG | ALPHA, R0 | RGB, FOG | RGB, ZERO);
    def[D_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, R0 | ALPHA, 0);
    rs_from_def(rs, def, 1, 3);
    src = check("fc_fog_explicit", rs, 0);
    expect("fc_fog_explicit", src, fogexpr[3], 1);
    free(src);
}

/* The def ModifyPixelShader builds in code: r0 = t2 (mask), r0 = t0 mux t1
   on r0.a's MSB, r0 *= v0, final out = r0 with alpha 1; then the variants
   the sample switches on at run time through SetRenderState. */
static void test_modify_pixel_shader(void)
{
    uint32_t def[60], rs[128];
    char *src;
    const uint32_t out_ab = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);

    memset(def, 0, sizeof def);
    def[D_COUNT] = PS_COMBINERCOUNT(3, MUX_MSB | UNIQUE_C0 | UNIQUE_C1);
    def[D_TEXMODES] = PS_TEXTUREMODES(TM_PROJECT2D, TM_PROJECT2D, TM_PROJECT2D, TM_NONE);
    def[D_RGBIN + 0] = PS_COMBINERINPUTS(T2 | RGB, ONE | RGB, ZERO | RGB, ZERO | RGB);
    def[D_ALPHAIN + 0] = PS_COMBINERINPUTS(T2 | BLUE, ONE | ALPHA, ZERO | ALPHA, ZERO | ALPHA);
    def[D_RGBOUT + 0] = out_ab;
    def[D_ALPHAOUT + 0] = out_ab;
    def[D_RGBIN + 1] = PS_COMBINERINPUTS(T0 | RGB, ONE | RGB, T1 | RGB, ONE | RGB);
    def[D_ALPHAIN + 1] = PS_COMBINERINPUTS(T0 | ALPHA, ONE | ALPHA, T1 | ALPHA, ONE | ALPHA);
    def[D_RGBOUT + 1] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, R0, IDENTITY | AB_CD_MUX);
    def[D_ALPHAOUT + 1] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, R0, IDENTITY | AB_CD_MUX);
    def[D_RGBIN + 2] = PS_COMBINERINPUTS(R0 | RGB, V0 | RGB, ZERO | RGB, ZERO | RGB);
    def[D_ALPHAIN + 2] = PS_COMBINERINPUTS(R0 | ALPHA, V0 | ALPHA, ZERO | ALPHA, ZERO | ALPHA);
    def[D_RGBOUT + 2] = out_ab;
    def[D_ALPHAOUT + 2] = out_ab;
    def[D_FCABCD] = PS_COMBINERINPUTS(ONE | RGB, R0 | RGB, ZERO | RGB, ZERO | RGB);
    def[D_FCEFG] = PS_COMBINERINPUTS(ZERO | RGB, ZERO | RGB, ONE | RGB | UNSIGNED_INVERT, 0);
    rs_from_def(rs, def, 0, 0);
    src = check("modify_ps", rs, 0);
    expect("modify_ps", src, "float Aa = max(t2.b, 0.0), Ba = 1.0", 1);
    expect("modify_ps", src, "bool mux = r0.a >= 0.5;", 1);
    expect("modify_ps", src, "vec3 SUM = mux ? CD : AB;", 1);
    expect("modify_ps", src, "r0.rgb = oSUM;", 1);
    expect("modify_ps", src, "vec3 fA = vec3(1.0);", 1);
    expect("modify_ps", src, "vec3 fB = max(r0.rgb, 0.0);", 1);
    expect("modify_ps", src, "gl_FragColor = clamp(vec4(fD + mix(fC, fB, fA), 1.0), 0.0, 1.0);", 1);
    free(src);

    /* fog blending switched on in the final combiner, two stages, inverted t0 */
    rs[D_FCABCD] = PS_COMBINERINPUTS(FOG | ALPHA, R0 | RGB, FOG | RGB, ZERO | RGB);
    rs[D_COUNT] = 2;
    rs[D_RGBIN + 1] = PS_COMBINERINPUTS(T0 | RGB | UNSIGNED_INVERT, ONE | RGB, T1 | RGB, ONE | RGB);
    rs[82] = 1;
    rs[83] = 3;
    src = check("modify_ps_fog", rs, 0);
    expect("modify_ps_fog", src, "/* stage 2 */", 0);
    expect("modify_ps_fog", src, "bool mux = mod(floor(r0.a * 255.0 + 0.5), 2.0) >= 1.0;", 1);
    expect("modify_ps_fog", src, "vec3 Argb = (1.0 - clamp(t0.rgb, 0.0, 1.0)), Brgb = vec3(1.0)", 1);
    expect("modify_ps_fog", src, "vec3 fA = max(fog.aaa, 0.0);", 1);
    expect("modify_ps_fog", src, "vec3 fC = max(fog.rgb, 0.0);", 1);
    expect("modify_ps_fog", src, "clamp((gl_Fog.end - gl_FogFragCoord) * gl_Fog.scale, 0.0, 1.0)", 1);
    free(src);
}

/* Link one generated shader against a vertex shader shaped like the ones
   vsh_translate produces, to catch interface problems (gl_TexCoord sizes,
   gl_FogFragCoord, secondary colour). */
static void test_link(void)
{
    uint32_t def[60], rs[128];
    char vpath[512], fpath[512], log[512];
    static const char *vs_full =
        "#version 120\n"
        "attribute vec4 v0;\n"
        "void main() {\n"
        "  gl_Position = v0;\n"
        "  gl_FrontColor = vec4(1.0); gl_FrontSecondaryColor = vec4(0.5);\n"
        "  gl_BackColor = vec4(1.0); gl_BackSecondaryColor = vec4(0.5);\n"
        "  gl_TexCoord[0] = v0; gl_TexCoord[1] = v0; gl_TexCoord[2] = v0; gl_TexCoord[3] = v0;\n"
        "  gl_FogFragCoord = v0.z;\n"
        "}\n";
    static const char *vs_t0 =
        "#version 120\n"
        "attribute vec4 v0;\n"
        "void main() {\n"
        "  gl_Position = v0;\n"
        "  gl_FrontColor = vec4(1.0);\n"
        "  gl_TexCoord[0] = v0;\n"
        "}\n";
    const char *vs[2] = { vs_full, vs_t0 };
    const char *vname[2] = { "stub_full", "stub_t0" };

    simple_def(def, T3, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOT_RFLCT_DIFF, TM_DOT_RFLCT_SPEC),
               PS_DOTMAPPING(0, 3, 3, 3), 0);
    def[D_FCABCD] = PS_COMBINERINPUTS(FOG | ALPHA, R0 | RGB, FOG | RGB, V1 | RGB);
    def[D_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, R0 | ALPHA, 0);
    rs_from_def(rs, def, 1, 3);
    char *src = psh_translate(rs);
    if (!src) { printf("FAIL link: NULL\n"); failures++; return; }
    snprintf(fpath, sizeof fpath, "%s/link_frag.frag", outdir);
    FILE *f = fopen(fpath, "w");
    fputs(src, f);
    fclose(f);
    free(src);
    for (int i = 0; i < 2; i++) {
        snprintf(vpath, sizeof vpath, "%s/%s.vert", outdir, vname[i]);
        snprintf(log, sizeof log, "%s/link_%s.log", outdir, vname[i]);
        f = fopen(vpath, "w");
        fputs(vs[i], f);
        fclose(f);
        if (run_glslcheck("link", vpath, fpath, log)) {
            printf("ok   link with %s\n", vname[i]);
            passes++;
        } else {
            printf("%s link with %s (see %s)\n", i == 0 ? "FAIL" : "note", vname[i], log);
            if (i == 0) failures++;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: psh_test <samples dir> <glslcheck> [outdir]\n");
        return 2;
    }
    glslcheck = argv[2];
    if (argc > 3) outdir = argv[3];
    mkdir(outdir, 0777);

    test_xpus(argv[1]);
    test_texture_modes();
    test_combiners();
    test_final_combiner();
    test_modify_pixel_shader();
    test_link();

    printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
