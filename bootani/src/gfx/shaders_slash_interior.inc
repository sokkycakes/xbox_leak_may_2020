// slash_interior: no assembly source survives, so this is decoded from the
// NV2A microcode in ani2/shaders.h (see tools/shaderdecode). The vertex
// shader cross-fades two vertical colour gradients (c4..c7) by the weight in
// c8; the pixel shader passes the colour through (final combiner unused).
static const char* kVS_slash_interior =
"void main() {\n"
"    xbox_init();\n"
"    vec4 oPos = vec4(dot(v0, c[0]), dot(v0, c[1]), dot(v0, c[2]), dot(v0, c[3]));\n"
"    vec3 r4 = (c[4].xyz - c[5].xyz) * v1.y + c[5].xyz;\n"
"    vec3 r7 = (c[6].xyz - c[7].xyz) * v1.y + c[7].xyz;\n"
"    vD0.xyz = (r4 * c[8].xyz + r7) - r7 * c[8].xyz;\n"
"    xbox_emit(oPos);\n"
"}\n";
static const char* kPS_slash_interior =
"void main() {\n"
"    xbox_output(vec4(cmb(sat(vD0)).rgb, 0.0));\n"
"}\n";
