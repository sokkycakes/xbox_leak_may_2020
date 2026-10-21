//
//  shaders.h
//
//  On the Xbox these were NV2A microcode blobs produced by xsasm (the
//  original bytes are kept in shaders_xbox_bin.h for reference). The portable
//  build hands the D3D8 layer a tag naming the GLSL translation instead
//  (gfx/shaders_glsl.cpp).
//
#pragma once

#define BOOTANI_SHADER_TAG(name, kind, tag) static const BYTE g_##name##_##kind[] = tag ":" #name;

BOOTANI_SHADER_TAG(greenfog,       xpu, "XPU")
BOOTANI_SHADER_TAG(scene_bump,     xpu, "XPU")
BOOTANI_SHADER_TAG(scene_phong,    xpu, "XPU")
BOOTANI_SHADER_TAG(scene_zr,       xpu, "XPU")
BOOTANI_SHADER_TAG(vblob,          xpu, "XPU")
BOOTANI_SHADER_TAG(vbloblet,       xpu, "XPU")
BOOTANI_SHADER_TAG(slash_interior, xpu, "XPU")
BOOTANI_SHADER_TAG(greenfog,       xvu, "XVU")
BOOTANI_SHADER_TAG(scene_bump,     xvu, "XVU")
BOOTANI_SHADER_TAG(scene_phong,    xvu, "XVU")
BOOTANI_SHADER_TAG(scene_zr,       xvu, "XVU")
BOOTANI_SHADER_TAG(vblob,          xvu, "XVU")
BOOTANI_SHADER_TAG(vbloblet,       xvu, "XVU")
BOOTANI_SHADER_TAG(slash_interior, xvu, "XVU")

#undef BOOTANI_SHADER_TAG
