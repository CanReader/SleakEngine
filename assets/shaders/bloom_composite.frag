#version 450
#extension GL_GOOGLE_include_directive : require

// ============================================================
// Bloom Composite Pass: HDR scene + SSR + bloom, then the output transform
// (ACES by default, or the soft curve of sleak/tonemap.glsl)
// ============================================================

#include "sleak/tonemap.glsl"

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneHDR;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(set = 0, binding = 2) uniform sampler2D ssrTex;

layout(push_constant) uniform BloomCompositePC {
    float bloomStrength;   // additive amount, or the blend share
    float exposure;        // exposure multiplier for HDR scene
    float gamma;           // 1.0 unless tonemapping is enabled
    uint  ssrEnabled;      // 0 skips the SSR fetch
    float sharpenStrength; // post-TAA RCAS strength, 0 = off, 1 = strongest
    uint  bloomMode;       // SLEAK_BLOOM_*
    uint  tonemapOperator; // SLEAK_TONEMAP_*
    float saturation;      // display-space saturation, 1 = identity
    float whiteCurve;      // soft curve exponent
    float dither;          // noise amplitude in display units, 0 = off
    float bloomNormalize;  // 1 / sum of the blend level weights
    float _pad0;
    vec4  whitePath;       // white-path matrix c, a, b, d
    vec4  whitePathInverse;
};

// BloomMode in GraphicsConfig.hpp
#define SLEAK_BLOOM_ADDITIVE 0u
#define SLEAK_BLOOM_BLEND 1u

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

    vec3 combined = hdr;
    if (bloomMode == SLEAK_BLOOM_BLEND) {
        // Energy-conserving: each pixel trades a share for the level blur
        vec3 bloom = texture(bloomTex, fragUV).rgb * bloomNormalize;
        combined = mix(hdr, bloom, clamp(bloomStrength, 0.0, 1.0));
    } else if (bloomStrength > 0.0) {
        // Add bloom on top of the HDR scene
        combined += texture(bloomTex, fragUV).rgb *
                    clamp(bloomStrength, 0.0, 1.0);
    }

    vec3 ldr;
    if (tonemapOperator == SLEAK_TONEMAP_ACES) {
        // Apply exposure before tone mapping.
        combined *= exposure;

        // Gamma stays 1.0 by default to match the OpenGL reference output
        ldr = SleakTonemapAces(combined);
        if (gamma != 1.0) ldr = pow(ldr, vec3(1.0 / max(gamma, 0.01)));
        if (saturation != 1.0)
            ldr = clamp(SleakSaturate(ldr, saturation), vec3(0.0), vec3(1.0));
    } else {
        SleakOutputParams o;
        o.op = tonemapOperator;
        o.exposure = exposure;
        o.gamma = gamma;
        o.saturation = saturation;
        o.dither = dither;
        o.curve = whiteCurve;
        o.white = whitePath;
        o.whiteInverse = whitePathInverse;
        ldr = SleakOutputTransform(combined, o);
    }
    if (dither > 0.0) ldr = SleakApplyDither(ldr, gl_FragCoord.xy, dither);

    outColor = vec4(ldr, 1.0);
}
