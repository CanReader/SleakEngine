#version 450

// ============================================================
// ACES Tonemapping Post-Process Shader - Vulkan Fragment
// HDR -> LDR with ACES filmic curve
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform PostProcessUBO {
    float Exposure;
    float Gamma;
    uint  TonemapEnabled;
    uint  _ppPad0;
};

layout(set = 1, binding = 0) uniform sampler2D sceneTexture;

// ACES filmic tone mapping (Narkowicz 2015 fit)
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(sceneTexture, fragUV).rgb;

    if (TonemapEnabled != 0u) {
        hdr *= Exposure;
        vec3 ldr = ACESFilm(hdr);
        ldr = pow(ldr, vec3(1.0 / Gamma));
        outColor = vec4(ldr, 1.0);
    } else {
        outColor = vec4(hdr, 1.0);
    }
}
