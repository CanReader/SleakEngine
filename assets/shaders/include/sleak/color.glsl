#ifndef SLEAK_COLOR_GLSL
#define SLEAK_COLOR_GLSL

// sRGB transfer function (IEC 61966-2-1)
#define SLEAK_SRGB_LINEAR_SLOPE 12.92
#define SLEAK_SRGB_DECODE_KNEE 0.04045
#define SLEAK_SRGB_ENCODE_KNEE 0.0031308
#define SLEAK_SRGB_SCALE 1.055
#define SLEAK_SRGB_OFFSET 0.055
#define SLEAK_SRGB_EXPONENT 2.4
// Rec. 601 luma weights
#define SLEAK_LUMA_601 vec3(0.299, 0.587, 0.114)
// Interleaved gradient noise weights (Jimenez 2014)
#define SLEAK_DITHER_IGN vec3(0.06711056, 0.00583715, 52.9829189)
// Dither noise is centred on zero
#define SLEAK_DITHER_CENTER 0.5
// Smallest saturation the inverse divides by
#define SLEAK_SATURATION_EPSILON 1e-4

/// sRGB encoding of a linear color.
vec3 SleakLinearToSrgb(vec3 c) {
    c = max(c, vec3(0.0));
    vec3 curve = SLEAK_SRGB_SCALE * pow(c, vec3(1.0 / SLEAK_SRGB_EXPONENT)) -
                 SLEAK_SRGB_OFFSET;
    return mix(curve, c * SLEAK_SRGB_LINEAR_SLOPE,
               lessThanEqual(c, vec3(SLEAK_SRGB_ENCODE_KNEE)));
}

/// Linear color of an sRGB-encoded value.
vec3 SleakSrgbToLinear(vec3 c) {
    c = max(c, vec3(0.0));
    vec3 curve = pow((c + SLEAK_SRGB_OFFSET) / SLEAK_SRGB_SCALE,
                     vec3(SLEAK_SRGB_EXPONENT));
    return mix(curve, c / SLEAK_SRGB_LINEAR_SLOPE,
               lessThanEqual(c, vec3(SLEAK_SRGB_DECODE_KNEE)));
}

/// Rec. 601 luma.
float SleakLuma601(vec3 c) { return dot(c, SLEAK_LUMA_601); }

/// Saturation around luma; 1 keeps the color, 0 is grey.
vec3 SleakSaturate(vec3 c, float amount) {
    return mix(vec3(SleakLuma601(c)), c, amount);
}

/// Inverse of SleakSaturate, which leaves luma unchanged.
vec3 SleakUnsaturate(vec3 c, float amount) {
    vec3 luma = vec3(SleakLuma601(c));
    return luma + (c - luma) / max(amount, SLEAK_SATURATION_EPSILON);
}

/// Adds zero-centred per-pixel noise of the given amplitude.
vec3 SleakApplyDither(vec3 c, vec2 pixel, float amplitude) {
    float noise = fract(SLEAK_DITHER_IGN.z *
                        fract(dot(pixel, SLEAK_DITHER_IGN.xy)));
    return c + (noise - SLEAK_DITHER_CENTER) * amplitude;
}

#endif
