#version 450

// ============================================================
// Bloom Composite Pass — HDR scene + SSR + bloom, ACES tonemap
// (gamma only when tonemapping is enabled, otherwise matches GL output)
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneHDR;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(set = 0, binding = 2) uniform sampler2D ssrTex;

layout(push_constant) uniform BloomCompositePC {
    float bloomStrength;   // bloom mix amount (typical 0.04..0.08 UE-style)
    float exposure;        // exposure multiplier for HDR scene
    float gamma;           // 1.0 unless tonemapping is enabled
    uint  ssrEnabled;      // 0 skips the SSR fetch
    float sharpenStrength; // post-TAA RCAS strength, 0 = off, 1 = strongest
    float _pad0;
    float _pad1;
    float _pad2;
};

// AMD FidelityFX FSR1 RCAS: caps the negative lobe so sharpening never rings
const float RCAS_LIMIT     = 0.25 - 1.0 / 16.0;
const float RCAS_EPSILON   = 1e-4;
const float PERCEPTUAL_MAX = 1.0 - 1e-4;

// Reversible per-channel Reinhard: RCAS expects values in [0, 1]
vec3 ToPerceptual(vec3 c)   { return c / (1.0 + c); }
vec3 FromPerceptual(vec3 c) { return c / (1.0 - min(c, vec3(PERCEPTUAL_MAX))); }

/// Robust contrast-adaptive sharpening of the centre texel e over its 4 neighbours.
vec3 SharpenRCAS(vec3 e, vec2 uv) {
    vec3 b = ToPerceptual(textureLodOffset(sceneHDR, uv, 0.0, ivec2( 0, -1)).rgb);
    vec3 d = ToPerceptual(textureLodOffset(sceneHDR, uv, 0.0, ivec2(-1,  0)).rgb);
    vec3 f = ToPerceptual(textureLodOffset(sceneHDR, uv, 0.0, ivec2( 1,  0)).rgb);
    vec3 h = ToPerceptual(textureLodOffset(sceneHDR, uv, 0.0, ivec2( 0,  1)).rgb);
    vec3 c = ToPerceptual(e);

    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, c) / max(4.0 * mx4, vec3(RCAS_EPSILON));
    vec3 hitMax = (1.0 - max(mx4, c)) / min(4.0 * mn4 - 4.0, vec3(-RCAS_EPSILON));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0))
               * sharpenStrength;

    vec3 sharp = (lobe * (b + d + f + h) + c) / (4.0 * lobe + 1.0);
    return FromPerceptual(clamp(sharp, vec3(0.0), vec3(PERCEPTUAL_MAX)));
}

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
    vec3 hdr = texture(sceneHDR, fragUV).rgb;
    if (sharpenStrength > 0.0) hdr = SharpenRCAS(hdr, fragUV);

    // SSR: ssr.rgb = reflectedColor * Fresnel * fades (premultiplied), ssr.a = fades only.
    // Unpremultiply to get reflectedColor * Fresnel, then lerp over the base scene using
    // ssr.a as coverage. This replaces (rather than adds to) the IBL-approximated specular,
    // so reflections are visible even on bright surfaces.
    if (ssrEnabled != 0u) {
        vec4 ssr = texture(ssrTex, fragUV);
        if (ssr.a > 0.001) {
            hdr = mix(hdr, ssr.rgb / max(ssr.a, 1e-4), min(ssr.a, 1.0));
        }
    }

    // Add bloom on top of the HDR scene. Bloom is a soft glow derived
    // from the bright parts of the scene; adding it preserves the base
    // luminance and only brightens hot spots (unlike mix, which dims
    // the scene by the bloom factor in non-glowing regions).
    vec3 combined = hdr;
    if (bloomStrength > 0.0)
        combined += texture(bloomTex, fragUV).rgb * clamp(bloomStrength, 0.0, 1.0);

    // Apply exposure before tone mapping.
    combined *= exposure;

    // Gamma stays 1.0 by default to match the OpenGL reference output
    vec3 ldr = ACESFilm(combined);
    if (gamma != 1.0) ldr = pow(ldr, vec3(1.0 / max(gamma, 0.01)));

    outColor = vec4(ldr, 1.0);
}
