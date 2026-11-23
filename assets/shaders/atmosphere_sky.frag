#version 450
#extension GL_GOOGLE_include_directive : require

// The atmosphere's sky on the forward path (Vulkan), drawn before the scene
// through the frame's output transform. The cloud noise is the forward
// layout's set 0 texture.

#define SLEAK_CLOUD_NOISE_VK 0
#include "sleak/atmosphere_pass.glsl"
#include "sleak/frame_output.glsl"

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 sky = SleakAtmosphereSkyPixel(SleakAtmosphereViewDir(fragUV));
    outColor = SleakFrameGuard(
        vec4(SleakOutputTransform(sky, SleakFrameOutput()), 1.0));
}
