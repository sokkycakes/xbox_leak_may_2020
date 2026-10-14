/*
 * NV2A shader translation: Xbox vertex program microcode and register
 * combiner state to GLSL 1.20 (OpenGL 2.1).
 *
 * Both translators return malloc'd shader source, or NULL when the input
 * uses something they cannot express; the caller logs that once and draws
 * nothing for that shader.  They are pure functions of their input: no GL
 * calls, no global state.
 */
#ifndef XBCOMPAT_NV2A_SHADERS_H
#define XBCOMPAT_NV2A_SHADERS_H

#include <stdint.h>

/*
 * Vertex programs.
 *
 * `code` points at the first 16-byte NV2A vertex program instruction (the
 * blob the title passes to D3DDevice_CreateVertexShader minus its one
 * dword header), `count` is the number of instructions.  The blob was
 * produced by the Xbox assembler, so it already ends with the viewport
 * epilogue: oPos is in screen space (x, y in pixels, z in 0..zscale, w is
 * the clip w, and x/y/z have already been divided by w).  R12 is a mirror
 * of oPos.
 *
 * Conventions of the generated GLSL:
 *   attribute vec4 v0 .. v15      vertex registers, bound to locations 0..15
 *   uniform vec4 c[192]           constant registers in hardware numbering
 *                                 (title register r lives in c[r + 96])
 *   uniform vec4 vp_scale, vp_offset  the viewport scale and offset the
 *                                 library writes to NV097_SET_VIEWPORT_*:
 *                                 scale = (W/2, -H/2, zscale*(maxZ-minZ), 0),
 *                                 offset = (X + W/2, Y + H/2, zscale*minZ, 0)
 *                                 (the library also keeps them in c[58] /
 *                                 c[59], which 192 constant titles may reuse)
 *   uniform float flip_y          1.0, or -1.0 when rendering into a texture
 *   gl_Position                   oPos converted back to OpenGL clip space:
 *                                   vec3 n = (oPos.xyz - vp_offset.xyz) / vp_scale.xyz;
 *                                   (guard vp_scale.z == 0: n.z = 0)
 *                                   gl_Position = vec4(n.x, n.y * flip_y,
 *                                                      2.0 * n.z - 1.0, 1.0) * oPos.w;
 *   gl_FrontColor / gl_FrontSecondaryColor      oD0 / oD1 (clamped to 0..1)
 *   gl_BackColor  / gl_BackSecondaryColor       oB0 / oB1 (oD0 / oD1 when
 *                                               the program never writes them)
 *   gl_TexCoord[0..3]             oT0 .. oT3
 *   uniform float fog_vertex_mode 0: legacy distance; 1/2/3: EXP/EXP2/LINEAR
 *   gl_FogFragCoord               oFog.x in legacy mode; otherwise vertex
 *                                 fog factor, unclamped until fragment use
 *   gl_PointSize                  oPts.x
 */
char *vsh_translate(const uint32_t *code, unsigned count);
extern char vsh_error[96];   /* why vsh_translate last returned NULL */

/*
 * Pixel shaders (register combiners).
 *
 * `rs` is the title's D3D render state array (D3D__RenderState).  The
 * translator may read only these entries, because the host caches the
 * fragment program keyed on them:
 *   rs[0 .. 56]   the D3DPIXELSHADERDEF as render states:
 *                 D3DRS_PSALPHAINPUTS0..7 (0..7), PSFINALCOMBINERINPUTSABCD (8),
 *                 PSFINALCOMBINERINPUTSEFG (9), PSCONSTANT0_0..7 (10..17),
 *                 PSCONSTANT1_0..7 (18..25), PSALPHAOUTPUTS0..7 (26..33),
 *                 PSRGBINPUTS0..7 (34..41), PSCOMPAREMODE (42),
 *                 PSFINALCOMBINERCONSTANT0/1 (43/44), PSRGBOUTPUTS0..7 (45..52),
 *                 PSCOMBINERCOUNT (53), [54 reserved], PSDOTMAPPING (55),
 *                 PSINPUTTEXTURE (56)
 *   rs[117]       D3DRS_PSTEXTUREMODES (5 bits per stage: PS_TEXTUREMODES_*)
 *   rs[82]        D3DRS_FOGENABLE
 *   rs[83]        D3DRS_FOGTABLEMODE (D3DFOG_NONE 0, EXP 1, EXP2 2, LINEAR 3)
 *   rs[93]        D3DRS_SPECULARENABLE (synthesized final combiner only)
 * Everything else that varies per draw comes in through uniforms.
 *
 * Conventions of the generated GLSL (declare only what the program uses):
 *   uniform sampler2D   tex0 .. tex3     texture unit i holds stage i's texture
 *   uniform samplerCube cube0 .. cube3   (the host binds whichever kind the
 *   uniform sampler3D   vol0 .. vol3      stage's texture mode needs)
 *   uniform vec4 tex_scale[4]            multiply stage i's coordinates by
 *                                        tex_scale[i].xyz before sampling
 *                                        (1 for swizzled textures, 1/size for
 *                                        linear ones, which use texel units)
 *   uniform vec4 c0[8], c1[8]            per-stage constants C0 / C1 as floats
 *   uniform vec4 fc0, fc1                final combiner constants
 *   uniform vec4 bump_env[4]             D3DTSS_BUMPENVMAT00, 01, 10, 11
 *   uniform vec2 bump_lum[4]             D3DTSS_BUMPENVLSCALE, BUMPENVLOFFSET
 *   uniform vec4 eye_vector              NV097_SET_EYE_VECTOR (unquantized)
 *   uniform vec4 key_color[4]            D3DTSS_COLORKEYCOLOR as RGBA
 *   gl_Color / gl_SecondaryColor         v0 / v1 (diffuse / specular)
 *   gl_TexCoord[0..3]                    stage 0..3 texture coordinates
 *   uniform float fog_vertex_mode       shared with vertex shader; nonzero
 *                                        means interpolate factor then clamp
 *   gl_FogFragCoord                      otherwise fog distance; with D3DRS_FOGENABLE
 *                                        the fog register is
 *                                        vec4(gl_Fog.color.rgb, f) where f
 *                                        comes from gl_Fog.start / end /
 *                                        density per D3DRS_FOGTABLEMODE
 *                                        (D3DFOG_NONE: f = gl_FogFragCoord)
 *   gl_FragColor                         the final combiner output
 * Alpha test is done by the host (fixed-function GL alpha test).
 */
/* Texture-stage controls that do not live in D3D__RenderState. Bitmasks
   use bit i for stage i; color_key contains D3DTCOLORKEYOP values 0..3. */
typedef struct {
    uint32_t alpha_kill;
    uint32_t color_key[4];
    uint32_t color_key_ignore_alpha;
    uint32_t bump_bgra;
} psh_options;
char *psh_translate_ex(const uint32_t *rs, const psh_options *options);
char *psh_translate(const uint32_t *rs);
/* Stages psh_translate samples as shadow buffers (depth textures), one bit each. */
extern uint32_t psh_shadow_stages;

/*
 * Run a state shader on the CPU: `code`/`count` as for vsh_translate, v0 is
 * the input vector (may be NULL), and `consts` are the 192 constant registers
 * in hardware numbering, which the program reads and writes in place.
 */
void vsh_run_state(const uint32_t *code, unsigned count, const float v0[4], float consts[192][4]);

#endif
