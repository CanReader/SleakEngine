#version 450
#extension GL_GOOGLE_include_directive : require

// Skybox texels are display-referred: under a non-ACES output transform in
// the deferred chain they are mapped so they still display as they did
// under the default ACES output.

#include "sleak/frame_output.glsl"

layout(location = 0) in vec3 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform samplerCube skyboxTexture;

void main() {
    vec4 sky = texture(skyboxTexture, fragTexCoord);
    if ((uFrame.Flags.x & SLEAK_FRAME_FLAG_DEFERRED) != 0u &&
        uFrame.Flags.z != SLEAK_TONEMAP_ACES)
        sky.rgb = SleakLegacyToScene(sky.rgb, SleakFrameOutput());
    outColor = sky;
}
