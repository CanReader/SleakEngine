#version 450 core
#extension GL_GOOGLE_include_directive : require

// The atmosphere's sky on the forward path (OpenGL), drawn before the scene
// through the frame's output transform.

#include "sleak/atmosphere_pass.glsl"
#include "sleak/frame_output.glsl"

in vec2 fragUV;
out vec4 outColor;

void main() {
    vec3 sky = SleakAtmosphereSkyPixel(SleakAtmosphereViewDir(fragUV));
    outColor = SleakFrameGuard(
        vec4(SleakOutputTransform(sky, SleakFrameOutput()), 1.0));
}
