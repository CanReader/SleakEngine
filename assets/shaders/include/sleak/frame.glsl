#ifndef SLEAK_FRAME_GLSL
#define SLEAK_FRAME_GLSL

#include "sleak/binding.glsl"

// FRAME_DATA_VERSION in ConstantBuffer.hpp; bump both on every append
#define SLEAK_FRAME_DATA_VERSION 2u

// Flags.x bits (FrameFlagBits in ConstantBuffer.hpp)
#define SLEAK_FRAME_FLAG_SSAO (1u << 0)
#define SLEAK_FRAME_FLAG_TAA (1u << 1)
#define SLEAK_FRAME_FLAG_SHADOWS (1u << 2)
#define SLEAK_FRAME_FLAG_DEFERRED (1u << 3)
#define SLEAK_FRAME_FLAG_GUARD (1u << 31)

// Drawn by SleakFrameGuard while a shader is stale
#define SLEAK_FRAME_GUARD_COLOR vec4(1.0, 0.0, 1.0, 1.0)

// Per-frame globals, std140, append only (FrameData in ConstantBuffer.hpp)
layout(std140, SLEAK_BINDING(SLEAK_FRAME_SET, SLEAK_FRAME_DATA_VK,
                             SLEAK_FRAME_DATA_GL)) uniform FrameDataUBO {
    uint  Version;
    uint  FrameIndex;
    float Time;             // seconds, frame / 60 in capture mode
    float DeltaTime;
    vec4  Screen;           // width, height, 1/width, 1/height
    vec4  Jitter;           // NDC after the Y flip: xy now, zw last frame
    vec4  CameraPos;        // xyz world, w near plane
    vec4  PrevCameraPos;    // xyz world, w far plane
    mat4  View;
    mat4  Proj;             // unjittered
    mat4  ViewProj;         // unjittered
    mat4  InvViewProjRel;   // NDC to world minus CameraPos
    mat4  PrevViewProjRel;  // world minus CameraPos to last frame's clip
    vec4  LightDir;         // xyz toward the light, w intensity
    vec4  LightColor;       // rgb, w shadow strength
    vec4  Ambient;          // rgb, w intensity
    uvec4 Flags;            // x SLEAK_FRAME_FLAG_* bits, y cascade count,
                            // z tonemap operator (SLEAK_TONEMAP_*)
    vec4  Output;           // exposure, gamma (1 = none), saturation, dither
    vec4  OutputCurve;      // x soft curve exponent, yzw reserved
    vec4  WhitePath;        // white-path matrix c, a, b, d
    vec4  WhitePathInverse; // its inverse, same layout
} uFrame;

// Game block: SLEAK_FRAME_USER_BLOCK Name { ... } instance;
#define SLEAK_FRAME_USER_BLOCK                                     \
    layout(std140, SLEAK_BINDING(SLEAK_FRAME_SET, SLEAK_FRAME_USER_VK, \
                                 SLEAK_FRAME_USER_GL)) uniform

/// True in Debug engine builds when this shader expects another FrameData.
bool SleakFrameDataStale() {
    return (uFrame.Flags.x & SLEAK_FRAME_FLAG_GUARD) != 0u &&
           uFrame.Version != SLEAK_FRAME_DATA_VERSION;
}

/// Magenta instead of color while SleakFrameDataStale holds.
vec4 SleakFrameGuard(vec4 color) {
    return SleakFrameDataStale() ? SLEAK_FRAME_GUARD_COLOR : color;
}

/// Degenerate clip position while SleakFrameDataStale holds.
vec4 SleakFrameGuardClip(vec4 clip) {
    return SleakFrameDataStale() ? vec4(0.0) : clip;
}

#endif
