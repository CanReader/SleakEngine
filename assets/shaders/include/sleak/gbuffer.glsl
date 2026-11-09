#ifndef SLEAK_GBUFFER_GLSL
#define SLEAK_GBUFFER_GLSL

// GBuffer RT3, RGBA16F cleared to 0: xy object motion, z game, w model id
#define SLEAK_SHADING_MODEL_ENGINE 0u
// Largest id a half float stores exactly
#define SLEAK_SHADING_MODEL_MAX 2047u
// Offset that rounds a stored id to the nearest integer
#define SLEAK_SHADING_MODEL_ROUND 0.5

/// RT3 value: NDC motion beyond camera reprojection (true previous NDC minus
/// reprojected, 0 when static), a game channel and a shading model id.
vec4 SleakEncodeMotionModel(vec2 objectMotion, float gameChannel,
                            uint shadingModel) {
    return vec4(objectMotion, gameChannel, float(shadingModel));
}

/// Shading model id of an RT3 texel; SLEAK_SHADING_MODEL_ENGINE for engine
/// materials and sky.
uint SleakShadingModel(vec4 motionModel) {
    return uint(motionModel.w + SLEAK_SHADING_MODEL_ROUND);
}

/// Object motion of an RT3 texel in NDC.
vec2 SleakObjectMotion(vec4 motionModel) { return motionModel.xy; }

#endif
