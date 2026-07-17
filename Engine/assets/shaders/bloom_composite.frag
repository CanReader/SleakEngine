#version 450

// ============================================================
// Bloom Composite Pass — HDR scene + SSR + bloom, ACES tonemap
// (no gamma — UNORM swapchain, matches GL reference output)
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneHDR;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(set = 0, binding = 2) uniform sampler2D ssrTex;

layout(push_constant) uniform BloomCompositePC {
    float bloomStrength;   // bloom mix amount (typical 0.04..0.08 UE-style)
    float exposure;        // exposure multiplier for HDR scene
    float _pad0;
    float _pad1;
};

// ACES Filmic tonemap (Narkowicz 2015)
vec3 ACESFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e),
                 vec3(0.0), vec3(1.0));
}

void main() {
    vec3 hdr   = texture(sceneHDR, fragUV).rgb;
    vec3 bloom = texture(bloomTex, fragUV).rgb;

    // SSR: ssr.rgb = reflectedColor * Fresnel * fades (premultiplied), ssr.a = fades only.
    // Unpremultiply to get reflectedColor * Fresnel, then lerp over the base scene using
    // ssr.a as coverage. This replaces (rather than adds to) the IBL-approximated specular,
    // so reflections are visible even on bright surfaces.
    vec4 ssr = texture(ssrTex, fragUV);
    if (ssr.a > 0.001) {
        hdr = mix(hdr, ssr.rgb / max(ssr.a, 1e-4), min(ssr.a, 1.0));
    }

    // Add bloom on top of the HDR scene. Bloom is a soft glow derived
    // from the bright parts of the scene; adding it preserves the base
    // luminance and only brightens hot spots (unlike mix, which dims
    // the scene by the bloom factor in non-glowing regions).
    vec3 combined = hdr + bloom * clamp(bloomStrength, 0.0, 1.0);

    // Apply exposure before tone mapping.
    combined *= exposure;

    // ACES only — no gamma, matching the OpenGL reference output
    vec3 ldr = ACESFilm(combined);

    outColor = vec4(ldr, 1.0);
}
