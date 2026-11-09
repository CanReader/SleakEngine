#ifndef SLEAK_SHADOW_LIGHT_GLSL
#define SLEAK_SHADOW_LIGHT_GLSL

#include "sleak/binding.glsl"

// Primary light, shadow and fog (ShadowLightUBO in ConstantBuffer.hpp),
// std140, append only. OpenGL gets a raw copy next to LightUBO/ShadowUBO.
layout(std140, SLEAK_BINDING(SLEAK_FRAME_SET, SLEAK_SHADOW_LIGHT_VK,
                             SLEAK_SHADOW_LIGHT_GL)) uniform ShadowLightUBO {
    vec4  uLightDir;          // xyz from the light, w normal bias
    vec4  uLightColor;        // rgb, w intensity
    vec4  uAmbient;           // rgb, w intensity
    vec4  uCameraPos;         // xyz world, w legacy copy of FrameData Time
    mat4  uLightVP;           // matrix the shadow map was drawn with
    float uShadowBias;
    float uShadowStrength;
    float uShadowTexelSize;
    float uLightSize;
    vec4  uFogColor;          // horizon
    float uFogStart;
    float uFogEnd;
    vec2  _fogPad;            // std140: vec2, never float[2]
    vec4  uFogZenithColor;
    float uHeightFogTop;
    float uHeightFogDensity;
    float uHeightFogFalloff;
    float uHeightFogEnabled;
    vec4  uExtraDir[3];       // xyz from the light, w unused
    vec4  uExtraColor[3];     // rgb, w intensity
    uint  uNumExtraLights;
    mat4  uNdcToShadow;       // no pad before it: std140 puts it at 320
};

#endif
