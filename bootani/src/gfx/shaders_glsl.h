//
//  shaders_glsl.h
//
#ifndef BOOTANI_SHADERS_GLSL_H
#define BOOTANI_SHADERS_GLSL_H

namespace bootani_gl {

const char* VertexShaderPrelude();
const char* PixelShaderPrelude();
// Body (main and helpers) of the GLSL translation of a named Xbox shader, or 0.
const char* FindVertexShader(const char* name);
const char* FindPixelShader(const char* name);

} // namespace bootani_gl

#endif
