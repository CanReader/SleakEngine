#version 450 core
#extension GL_GOOGLE_include_directive : require

// One face of the sky probe (OpenGL): the sky gradient and clouds through
// the frame's output transform, display-referred like the OpenGL snapshot.

#include "sleak/clouds.glsl"
#include "sleak/frame_output.glsl"

// Cube face being drawn, SLEAK_CUBE_FACE_* (OpenGLRenderer::RenderSkyProbe)
uniform uint uProbeFace;

in vec2 fragUV;
out vec4 outColor;

void main() {
    vec3 dir = SleakCubeFaceDirection(uProbeFace, fragUV);
    vec3 display = SleakOutputTransform(SleakAtmosphereProbeSky(dir),
                                        SleakFrameOutput());
    outColor = SleakAtmosphereStale() ? SLEAK_FRAME_GUARD_COLOR
                                      : SleakFrameGuard(vec4(display, 1.0));
}
