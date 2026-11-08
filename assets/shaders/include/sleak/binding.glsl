#ifndef SLEAK_BINDING_GLSL
#define SLEAK_BINDING_GLSL

// Slots shared with the C++ backends: Vulkan descriptor set 2 bindings
// (VulkanRenderer SET2_*) and OpenGL uniform buffer bindings
// (OpenGLRenderer *_UBO_BINDING)
#define SLEAK_FRAME_SET 2
#define SLEAK_SHADOW_LIGHT_VK 0
#define SLEAK_FRAME_DATA_VK 1
#define SLEAK_FRAME_USER_VK 2
#define SLEAK_CASCADES_VK 3
#define SLEAK_ATMOSPHERE_VK 4
#define SLEAK_FRAME_DATA_GL 11
#define SLEAK_FRAME_USER_GL 12
#define SLEAK_CASCADES_GL 13
#define SLEAK_SHADOW_LIGHT_GL 14
#define SLEAK_ATMOSPHERE_GL 15

// Vulkan takes a set and binding, OpenGL one flat buffer binding
#ifdef VULKAN
#define SLEAK_BINDING(set_, vk_, gl_) set = set_, binding = vk_
#else
#define SLEAK_BINDING(set_, vk_, gl_) binding = gl_
#endif

/// NDC xy of a screen UV; Vulkan rows run top-down, OpenGL bottom-up.
vec2 SleakUvToNdc(vec2 uv) {
#ifdef VULKAN
    return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
#else
    return uv * 2.0 - 1.0;
#endif
}

/// Screen UV of an NDC xy; inverse of SleakUvToNdc.
vec2 SleakNdcToUv(vec2 ndc) {
#ifdef VULKAN
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
#else
    return ndc * 0.5 + 0.5;
#endif
}

/// Stored depth of an NDC z; OpenGL maps the engine's [0,1] into [0.5,1].
float SleakNdcToDepth(float ndcZ) {
#ifdef VULKAN
    return ndcZ;
#else
    return ndcZ * 0.5 + 0.5;
#endif
}

/// NDC z of a stored depth; inverse of SleakNdcToDepth.
float SleakDepthToNdc(float depth) {
#ifdef VULKAN
    return depth;
#else
    return depth * 2.0 - 1.0;
#endif
}

/// Clip position as the backend rasterizes it; Vulkan flips Y.
vec4 SleakClipToTarget(vec4 clip) {
#ifdef VULKAN
    return vec4(clip.x, -clip.y, clip.z, clip.w);
#else
    return clip;
#endif
}

#endif
