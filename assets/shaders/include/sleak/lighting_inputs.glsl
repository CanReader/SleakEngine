#ifndef SLEAK_LIGHTING_INPUTS_GLSL
#define SLEAK_LIGHTING_INPUTS_GLSL

#include "sleak/binding.glsl"
#include "sleak/frame.glsl"
#include "sleak/gbuffer.glsl"
#include "sleak/shadow_light.glsl"

// Inputs of every deferred lighting fragment shader (SetLightingShader).
// outColor: linear HDR on Vulkan; display-ready on OpenGL, which also
// writes gl_FragDepth and discards sky.

// Vulkan sets (VulkanRenderer m_lightingPipelineLayout)
#define SLEAK_LIGHTING_SAMPLER_SET 0
#define SLEAK_LIGHTING_CB_SET 1
#define SLEAK_LIGHTING_IBL_SET 3

// Vulkan set 0 bindings (VulkanRenderer LIGHTING_*_BINDING); 5 is retired
#define SLEAK_ALBEDO_AO_VK 0
#define SLEAK_NORMAL_ROUGH_VK 1
#define SLEAK_METAL_EMIT_VK 2
#define SLEAK_DEPTH_VK 3
#define SLEAK_SHADOW_MAP_VK 4
#define SLEAK_SSAO_VK 6
#define SLEAK_MOTION_MODEL_VK 7
// Vulkan set 1 and set 3 bindings
#define SLEAK_DEFERRED_CB_VK 0
#define SLEAK_IBL_IRRADIANCE_VK 0
#define SLEAK_IBL_PREFILTER_VK 1
#define SLEAK_IBL_BRDF_VK 2
#define SLEAK_IBL_SETTINGS_VK 3

// OpenGL texture units (OpenGLRenderer ExecuteDeferredLightingPass)
#define SLEAK_SHADOW_MAP_GL 3
#define SLEAK_ALBEDO_AO_GL 8
#define SLEAK_NORMAL_ROUGH_GL 9
#define SLEAK_METAL_EMIT_GL 10
#define SLEAK_DEPTH_GL 11
#define SLEAK_MOTION_MODEL_GL 12
#define SLEAK_SSAO_GL 13
#define SLEAK_IBL_IRRADIANCE_GL 14
#define SLEAK_IBL_PREFILTER_GL 15
#define SLEAK_IBL_BRDF_GL 16
// OpenGL uniform buffer bindings
#define SLEAK_LIGHT_CB_GL 2
#define SLEAK_SHADOW_CB_GL 5
#define SLEAK_DEFERRED_CB_GL 6
#define SLEAK_IBL_SETTINGS_GL 10
// MAX_LIGHTS in ConstantBuffer.hpp
#define SLEAK_MAX_LIGHTS 16

#ifdef VULKAN
layout(location = 0) in vec2 fragUV;  // 0,0 at the top-left
layout(location = 0) out vec4 outColor;
#else
in vec2 fragUV;  // 0,0 at the bottom-left
out vec4 outColor;
#endif

// GBuffer, depth, shadow map, screen-space AO
layout(SLEAK_BINDING(SLEAK_LIGHTING_SAMPLER_SET, SLEAK_ALBEDO_AO_VK,
                     SLEAK_ALBEDO_AO_GL)) uniform sampler2D gAlbedoAO;
layout(SLEAK_BINDING(SLEAK_LIGHTING_SAMPLER_SET, SLEAK_NORMAL_ROUGH_VK,
                     SLEAK_NORMAL_ROUGH_GL)) uniform sampler2D gNormalRough;
layout(SLEAK_BINDING(SLEAK_LIGHTING_SAMPLER_SET, SLEAK_METAL_EMIT_VK,
                     SLEAK_METAL_EMIT_GL)) uniform sampler2D gMetalEmit;
layout(SLEAK_BINDING(SLEAK_LIGHTING_SAMPLER_SET, SLEAK_DEPTH_VK,
                     SLEAK_DEPTH_GL)) uniform sampler2D gDepth;
layout(SLEAK_BINDING(SLEAK_LIGHTING_SAMPLER_SET, SLEAK_SHADOW_MAP_VK,
                     SLEAK_SHADOW_MAP_GL)) uniform sampler2DShadow gShadow;
layout(SLEAK_BINDING(SLEAK_LIGHTING_SAMPLER_SET, SLEAK_SSAO_VK,
                     SLEAK_SSAO_GL)) uniform sampler2D gSSAO;
layout(SLEAK_BINDING(SLEAK_LIGHTING_SAMPLER_SET, SLEAK_MOTION_MODEL_VK,
                     SLEAK_MOTION_MODEL_GL)) uniform sampler2D gMotionModel;

// Camera reconstruction (DeferredCBData in ConstantBuffer.hpp)
layout(std140, SLEAK_BINDING(SLEAK_LIGHTING_CB_SET, SLEAK_DEFERRED_CB_VK,
                             SLEAK_DEFERRED_CB_GL)) uniform DeferredCB {
    mat4  InvViewProj;  // unjittered NDC to world
    float ScreenW;
    float ScreenH;
    float NearP;
    float FarP;
};

// Image-based lighting (stub textures on Vulkan, iblEnabled stays 0)
layout(SLEAK_BINDING(SLEAK_LIGHTING_IBL_SET, SLEAK_IBL_IRRADIANCE_VK,
                     SLEAK_IBL_IRRADIANCE_GL))
    uniform samplerCube iblIrradiance;
layout(SLEAK_BINDING(SLEAK_LIGHTING_IBL_SET, SLEAK_IBL_PREFILTER_VK,
                     SLEAK_IBL_PREFILTER_GL)) uniform samplerCube iblPrefilter;
layout(SLEAK_BINDING(SLEAK_LIGHTING_IBL_SET, SLEAK_IBL_BRDF_VK,
                     SLEAK_IBL_BRDF_GL)) uniform sampler2D iblBrdfLUT;
layout(std140, SLEAK_BINDING(SLEAK_LIGHTING_IBL_SET, SLEAK_IBL_SETTINGS_VK,
                             SLEAK_IBL_SETTINGS_GL)) uniform IBLSettings {
    uint  iblEnabled;
    float iblIntensity;
    float maxReflectionLOD;
    float _iblPad;
};

#ifndef VULKAN
// One entry of the OpenGL light list (LightGPUEntry in ConstantBuffer.hpp)
struct SleakLightData {
    vec3  Position;  uint Type;
    vec3  Direction; float Intensity;
    vec3  Color;     float Range;
    float SpotInnerCos; float SpotOuterCos;
    float AreaWidth;    float AreaHeight;
};

// OpenGL lights, ambient and fog (LightCBData in ConstantBuffer.hpp)
layout(std140, binding = SLEAK_LIGHT_CB_GL) uniform LightUBO {
    vec3  CameraPos;
    uint  NumActiveLights;
    vec3  AmbientColor;
    float AmbientIntensity;
    vec4  FogColor;  // horizon
    float FogStart;
    float FogEnd;
    float _lightPad0, _lightPad1;
    SleakLightData Lights[SLEAK_MAX_LIGHTS];
    vec4  FogColorZenith;
    float HeightFogTop;
    float HeightFogDensity;
    float HeightFogFalloff;
    float HeightFogEnabled;
};

// OpenGL shadow parameters (PCSSShadowGPUData in ConstantBuffer.hpp)
layout(std140, binding = SLEAK_SHADOW_CB_GL) uniform ShadowUBO {
    mat4  ShadowLightVP;  // matrix the map was drawn with
    float ShadowBias;
    float ShadowStrength;
    float ShadowTexelSize;
    float ShadowLightSize;
    uint  PCSSEnabled;
    uint  ShadowMapEnabled;
    float _shadowPad0, _shadowPad1;
    mat4  NdcToShadow;
};
#endif

/// True when screen-space AO ran this frame. Vulkan binds white to gSSAO
/// while it is off; OpenGL may leave the unit empty.
bool SleakSSAOEnabled() {
    return (uFrame.Flags.x & SLEAK_FRAME_FLAG_SSAO) != 0u;
}

/// RT3 of a pixel: object motion, game channel, shading model id.
vec4 SleakReadMotionModel(ivec2 pixel) {
    return texelFetch(gMotionModel, pixel, 0);
}

#endif
