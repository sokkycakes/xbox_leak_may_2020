/*
 * xbcompat-owned fixed-function lighting for the verified ordinary-light
 * subset. All numeric inputs are eye-space values, packed in vec4s:
 *  0: specular enabled, normalize normals, local viewer, material power
 *  1: diffuse/ambient/specular/emissive source (0 material, 1 primary, 2 secondary)
 *  2: scene ambient
 *  3..6: diffuse, ambient, specular, emissive material
 *  7..8: reserved
 *  9 + 5*i: light position (point) or direction toward light (directional);
 *           W selects disabled=0, point=1, directional=3
 *      +1: ambient, +2: diffuse, +3: specular, +4: a0/a1/a2/range
 *
 * Selection in d3d8.c retains the legacy path for unsupported FF states.
 * This is independently implemented from the sanitized behavior specification
 * and the Xbox D3D API's state definitions; no reference implementation code
 * was used.
 */
#ifndef XBCOMPAT_FF_VSH_H
#define XBCOMPAT_FF_VSH_H
#define FF_LIGHTING_VECTORS 49
static const char ff_lighting_source[] =
"#version 120\n"
"uniform vec4 ff_lighting[49];\n"
"\n"
"vec3 ff_unit(vec3 v)\n"
"{\n"
"    float m = length(v);\n"
"    return m > 0.0 ? v / m : vec3(0.0);\n"
"}\n"
"\n"
"vec4 ff_material(float source, vec4 material)\n"
"{\n"
"    if (source == 1.0) return gl_Color;\n"
"    if (source == 2.0) return gl_SecondaryColor;\n"
"    return material;\n"
"}\n"
"\n"
"void main()\n"
"{\n"
"    vec4 eye = gl_ModelViewMatrix * gl_Vertex;\n"
"    vec3 position = eye.xyz / eye.w;\n"
"    vec3 normal = gl_NormalMatrix * gl_Normal;\n"
"    if (ff_lighting[0].y != 0.0) normal = ff_unit(normal);\n"
"    vec3 viewer = ff_lighting[0].z != 0.0 ? ff_unit(-position) : vec3(0.0, 0.0, 1.0);\n"
"    vec4 sources = ff_lighting[1];\n"
"    vec4 diffuse = ff_material(sources.x, ff_lighting[3]);\n"
"    vec4 ambient = ff_material(sources.y, ff_lighting[4]);\n"
"    vec4 specular = ff_material(sources.z, ff_lighting[5]);\n"
"    vec4 emissive = ff_material(sources.w, ff_lighting[6]);\n"
"    vec3 primary = emissive.rgb + ambient.rgb * ff_lighting[2].rgb;\n"
"    vec3 secondary = vec3(0.0);\n"
"    for (int i = 0; i < 8; i++) {\n"
"        int base = 9 + 5 * i;\n"
"        vec4 light = ff_lighting[base];\n"
"        if (light.w != 0.0) {\n"
"            vec3 toward = light.w == 3.0 ? light.xyz : light.xyz - position;\n"
"            float distance = length(toward);\n"
"            vec4 attenuation = ff_lighting[base + 4];\n"
"            float amount = 1.0;\n"
"            if (light.w != 3.0) {\n"
"                if (distance > attenuation.w) amount = 0.0;\n"
"                else amount = 1.0 / dot(attenuation.xyz, vec3(1.0, distance, distance * distance));\n"
"            }\n"
"            vec3 direction = ff_unit(toward);\n"
"            float lambert = max(dot(normal, direction), 0.0);\n"
"            primary += amount * (ambient.rgb * ff_lighting[base + 1].rgb\n"
"                             + diffuse.rgb * ff_lighting[base + 2].rgb * lambert);\n"
"            if (ff_lighting[0].x != 0.0 && lambert > 0.0) {\n"
"                float alignment = max(dot(normal, ff_unit(direction + viewer)), 0.0);\n"
"                float shine = ff_lighting[0].w == 0.0 ? 1.0 : pow(alignment, ff_lighting[0].w);\n"
"                secondary += amount * specular.rgb * ff_lighting[base + 3].rgb * shine;\n"
"            }\n"
"        }\n"
"    }\n"
"    gl_FrontColor = clamp(vec4(primary, diffuse.a), 0.0, 1.0);\n"
"    gl_FrontSecondaryColor = clamp(vec4(secondary, 0.0), 0.0, 1.0);\n"
"    gl_BackColor = gl_FrontColor;\n"
"    gl_BackSecondaryColor = gl_FrontSecondaryColor;\n"
"    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
"    gl_FogFragCoord = abs(position.z);\n"
"    gl_PointSize = gl_Point.size;\n"
"    gl_TexCoord[0] = gl_TextureMatrix[0] * gl_MultiTexCoord0;\n"
"    gl_TexCoord[1] = gl_TextureMatrix[1] * gl_MultiTexCoord1;\n"
"    gl_TexCoord[2] = gl_TextureMatrix[2] * gl_MultiTexCoord2;\n"
"    gl_TexCoord[3] = gl_TextureMatrix[3] * gl_MultiTexCoord3;\n"
"}\n";
#endif
