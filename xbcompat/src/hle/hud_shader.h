/*
 * A small GLSL program for HUD draws.
 *
 * Titles draw their HUD, text and menus as pre-transformed quads with one
 * texture modulated by the vertex color: stage 0 MODULATE of TEXTURE and
 * DIFFUSE, the later stages off, no fog, lighting, specular, alpha test,
 * stencil or point sprites (hud_shader_supported in d3d8.c checks all of
 * it).  Such a draw needs none of the fixed-function pipeline, and on the
 * Pi's Mesa the fixed-function program it would otherwise get is rebuilt
 * whenever an unrelated state changes; this one is compiled once and
 * stays bound across a run of HUD draws.
 *
 * The vertex shader takes the transform and texture matrix the draw set
 * up, so it lands where the fixed-function draw would.  Opt in with
 * XBCOMPAT_HUD_SHADER=1.
 *
 * Included by d3d8.c (hud_shader_bind).
 */
#ifndef XBCOMPAT_HUD_SHADER_H
#define XBCOMPAT_HUD_SHADER_H
static const char hud_vs_source[] = "#version 120\n"
    "varying vec2 hud_uv;\n"
    "void main() {\n"
    "    gl_Position = ftransform();\n"
    "    gl_FrontColor = gl_Color;\n"
    "    hud_uv = (gl_TextureMatrix[0] * gl_MultiTexCoord0).xy;\n"
    "}\n";

static const char hud_fs_source[] = "#version 120\n"
    "uniform sampler2D hud_tex;\n"
    "varying vec2 hud_uv;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(hud_tex, hud_uv) * gl_Color;\n"
    "}\n";
#endif
