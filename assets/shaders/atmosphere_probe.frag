#version 450
#extension GL_GOOGLE_include_directive : require

// One face of the sky probe (Vulkan): the sky gradient and clouds without
// discs, stars or glare, scene-linear like the scene-read snapshot.

#include "sleak/clouds.glsl"

// Cube face this pipeline draws, SLEAK_CUBE_FACE_*
layout(constant_id = 0) const uint SLEAK_PROBE_FACE = 0u;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 dir = SleakCubeFaceDirection(SLEAK_PROBE_FACE, fragUV);
    vec3 sky = SleakAtmosphereProbeSky(dir);
    outColor = SleakAtmosphereStale() ? SLEAK_FRAME_GUARD_COLOR
                                      : vec4(sky, 1.0);
}
