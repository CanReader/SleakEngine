#ifndef SLEAK_TONEMAP_GLSL
#define SLEAK_TONEMAP_GLSL

#include "sleak/color.glsl"

// TonemapOperator in GraphicsConfig.hpp
#define SLEAK_TONEMAP_ACES 0u
#define SLEAK_TONEMAP_SOFT 1u
// ACES fit (Narkowicz 2015): x (A x + B) / (x (C x + D) + E)
#define SLEAK_TONEMAP_ACES_A 2.51
#define SLEAK_TONEMAP_ACES_B 0.03
#define SLEAK_TONEMAP_ACES_C 2.43
#define SLEAK_TONEMAP_ACES_D 0.59
#define SLEAK_TONEMAP_ACES_E 0.14
// Largest outputs the inverses accept; both curves flatten toward 1
#define SLEAK_TONEMAP_ACES_INVERSE_MAX 0.985
#define SLEAK_TONEMAP_SOFT_INVERSE_MAX 0.9995
// Floors for the exposure divisor and the gamma
#define SLEAK_TONEMAP_EPSILON 1e-4
#define SLEAK_TONEMAP_MIN_GAMMA 0.01
// Soft curve exponent with a closed form: x / sqrt(1 + x^2)
#define SLEAK_TONEMAP_SOFT_SQRT_CURVE 2.0

/// Output transform settings (OutputTransformParams in OutputTransform.hpp).
struct SleakOutputParams {
    uint op;             // SLEAK_TONEMAP_*
    float exposure;      // linear multiplier before the curve
    float gamma;         // display encode 1/gamma, 1 = none
    float saturation;    // display-space saturation, 1 = identity
    float dither;        // noise amplitude in display units, 0 = off
    float curve;         // soft curve exponent p
    vec4 white;          // white-path matrix entries c, a, b, d
    vec4 whiteInverse;   // its inverse, same layout
};

/// ACES fit, clamped to [0, 1].
vec3 SleakTonemapAces(vec3 x) {
    return clamp((x * (SLEAK_TONEMAP_ACES_A * x + SLEAK_TONEMAP_ACES_B)) /
                     (x * (SLEAK_TONEMAP_ACES_C * x + SLEAK_TONEMAP_ACES_D) +
                      SLEAK_TONEMAP_ACES_E),
                 vec3(0.0), vec3(1.0));
}

/// Positive root of the ACES fit for outputs below its asymptote.
vec3 SleakTonemapAcesInverse(vec3 y) {
    y = clamp(y, vec3(0.0), vec3(SLEAK_TONEMAP_ACES_INVERSE_MAX));
    vec3 a = SLEAK_TONEMAP_ACES_C * y - SLEAK_TONEMAP_ACES_A;
    vec3 b = SLEAK_TONEMAP_ACES_D * y - SLEAK_TONEMAP_ACES_B;
    vec3 c = SLEAK_TONEMAP_ACES_E * y;
    return (-b - sqrt(max(b * b - 4.0 * a * c, vec3(0.0)))) / (2.0 * a);
}

/// Soft shoulder x / (1 + x^p)^(1/p) per channel.
vec3 SleakSoftCurve(vec3 x, float p) {
    x = max(x, vec3(0.0));
    if (p == SLEAK_TONEMAP_SOFT_SQRT_CURVE)
        return x * inversesqrt(vec3(1.0) + x * x);
    return x * pow(vec3(1.0) + pow(x, vec3(p)), vec3(-1.0 / p));
}

/// Inverse soft shoulder y / (1 - y^p)^(1/p).
vec3 SleakSoftCurveInverse(vec3 y, float p) {
    y = clamp(y, vec3(0.0), vec3(SLEAK_TONEMAP_SOFT_INVERSE_MAX));
    if (p == SLEAK_TONEMAP_SOFT_SQRT_CURVE)
        return y * inversesqrt(vec3(1.0) - y * y);
    return y * pow(vec3(1.0) - pow(y, vec3(p)), vec3(-1.0 / p));
}

/// Symmetric white-path matrix from its four distinct entries.
mat3 SleakWhitePathMatrix(vec4 m) {
    return mat3(m.x, m.y, m.z, m.y, m.w, m.y, m.z, m.y, m.x);
}

/// Linear scene color to display values in [0, 1], without dither.
vec3 SleakOutputTransform(vec3 scene, SleakOutputParams o) {
    vec3 x = max(scene, vec3(0.0)) * o.exposure;
    vec3 y;
    if (o.op == SLEAK_TONEMAP_SOFT) {
        vec3 bent = SleakWhitePathMatrix(o.white) * x;
        y = SleakWhitePathMatrix(o.whiteInverse) *
            SleakSoftCurve(bent, o.curve);
        y = clamp(y, vec3(0.0), vec3(1.0));
    } else {
        y = SleakTonemapAces(x);
    }
    if (o.gamma != 1.0)
        y = pow(y, vec3(1.0 / max(o.gamma, SLEAK_TONEMAP_MIN_GAMMA)));
    if (o.saturation != 1.0)
        y = clamp(SleakSaturate(y, o.saturation), vec3(0.0), vec3(1.0));
    return y;
}

/// Scene color that SleakOutputTransform maps to a display value.
vec3 SleakOutputTransformInverse(vec3 display, SleakOutputParams o) {
    vec3 y = clamp(display, vec3(0.0), vec3(1.0));
    if (o.saturation != 1.0)
        y = clamp(SleakUnsaturate(y, o.saturation), vec3(0.0), vec3(1.0));
    if (o.gamma != 1.0)
        y = pow(y, vec3(max(o.gamma, SLEAK_TONEMAP_MIN_GAMMA)));
    vec3 x;
    if (o.op == SLEAK_TONEMAP_SOFT) {
        vec3 bent = SleakWhitePathMatrix(o.white) * y;
        x = SleakWhitePathMatrix(o.whiteInverse) *
            SleakSoftCurveInverse(bent, o.curve);
    } else {
        x = SleakTonemapAcesInverse(y);
    }
    return max(x, vec3(0.0)) / max(o.exposure, SLEAK_TONEMAP_EPSILON);
}

/// Scene color that displays like a color authored for the default ACES
/// output (exposure 1, no encode); unchanged while the operator is ACES.
vec3 SleakLegacyToScene(vec3 legacyHdr, SleakOutputParams o) {
    if (o.op == SLEAK_TONEMAP_ACES) return legacyHdr;
    return SleakOutputTransformInverse(SleakTonemapAces(legacyHdr), o);
}

/// Legacy-authored HDR value that displays like a scene color does now.
vec3 SleakSceneToLegacy(vec3 scene, SleakOutputParams o) {
    if (o.op == SLEAK_TONEMAP_ACES) return scene;
    return SleakTonemapAcesInverse(SleakOutputTransform(scene, o));
}

/// Display value under the active output of a legacy ACES display value.
vec3 SleakLegacyDisplayToOutput(vec3 legacyDisplay, SleakOutputParams o) {
    if (o.op == SLEAK_TONEMAP_ACES) return legacyDisplay;
    return SleakOutputTransform(SleakTonemapAcesInverse(legacyDisplay), o);
}

#endif
