#version 450 core

// ============================================================
// SSAO Blur Pass - OpenGL Fragment
// Simple 4x4 box blur to smooth SSAO noise
// ============================================================

in vec2 fragUV;
out float outColor;

layout(binding = 0) uniform sampler2D ssaoInput;

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(ssaoInput, 0));

    float result = 0.0;
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            result += texture(ssaoInput, fragUV + offset).r;
        }
    }
    result /= 16.0;

    outColor = result;
}
