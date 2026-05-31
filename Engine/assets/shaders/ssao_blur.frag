#version 450

// ============================================================
// SSAO Bilateral Blur — depth-aware to avoid bleeding across edges
// 4x4 cross-shaped kernel with depth falloff weighting.
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 0) out float outOcclusion;

layout(set = 0, binding = 0) uniform sampler2D ssaoInput;
layout(set = 0, binding = 1) uniform sampler2D gDepth;

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(ssaoInput, 0));

    float centerDepth = texture(gDepth, fragUV).r;
    if (centerDepth >= 0.9999) {
        // Sky — no blur required, preserve 1.0.
        outOcclusion = 1.0;
        return;
    }

    // 4x4 symmetric box blur with bilateral depth weighting.
    // Depth weight: exp(-|dz| / sigma) — preserves crevice darkness
    // while suppressing bleed across geometric edges.
    const float sigma     = 0.0005; // depth-delta tolerance (in normalized depth units)
    const float invSigma  = 1.0 / sigma;

    float weightSum = 0.0;
    float sum       = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2  off       = vec2(float(x), float(y)) * texelSize;
            float sampleAO  = texture(ssaoInput, fragUV + off).r;
            float sampleDz  = texture(gDepth,    fragUV + off).r;
            float dz        = abs(sampleDz - centerDepth);
            float w         = exp(-dz * invSigma);
            sum       += sampleAO * w;
            weightSum += w;
        }
    }

    outOcclusion = (weightSum > 0.0) ? (sum / weightSum) : texture(ssaoInput, fragUV).r;
}
