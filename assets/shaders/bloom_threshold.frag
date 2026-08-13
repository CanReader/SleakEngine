#version 450

// ============================================================
// Bloom Threshold/Prefilter Pass (UE4-style soft-knee curve)
// Extracts bright parts of HDR scene into the bloom chain's top mip.
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneHDR;

layout(push_constant) uniform BloomThresholdPC {
    float threshold;   // HDR threshold (linear luminance)
    float knee;        // soft-knee smoothness (0 = hard, 1 = soft)
    float _pad0;
    float _pad1;
};

void main() {
    vec3 hdr = texture(sceneHDR, fragUV).rgb;

    // Firefly clamp — lone bright pixels ruin the bloom kernel.
    // 65504 is the max half-float value; clamp well below to keep gaussians stable.
    hdr = min(hdr, vec3(1000.0));

    float brightness = max(hdr.r, max(hdr.g, hdr.b));

    // Quadratic soft-knee curve (COD/UE4 style):
    //   soft = clamp(brightness - threshold + knee, 0, 2*knee)
    //   soft = soft * soft / (4*knee + eps)
    //   contribution = max(soft, brightness - threshold) / max(brightness, eps)
    float kneeDelta = knee * threshold;
    float soft = brightness - threshold + kneeDelta;
    soft = clamp(soft, 0.0, 2.0 * kneeDelta);
    soft = soft * soft / (4.0 * kneeDelta + 1e-4);

    float contribution = max(soft, brightness - threshold);
    contribution /= max(brightness, 1e-4);

    outColor = vec4(hdr * contribution, 1.0);
}
