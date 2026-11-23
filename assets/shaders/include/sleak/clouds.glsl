#ifndef SLEAK_CLOUDS_GLSL
#define SLEAK_CLOUDS_GLSL

#include "sleak/atmosphere.glsl"

// Cloud layer of the atmosphere: a flat-based slab of 2D noise shaped by a
// height profile and lit by a single-scatter approximation (no light march).
// Only lighting-layout shaders include it: the composite, the probes and the
// OpenGL lighting stems; the forward sky takes the noise as its texture.

// Noise tile (CloudNoise.cpp): Vulkan set 0 binding 5 of the lighting
// layout (VulkanRenderer LIGHTING_CLOUD_NOISE_BINDING), OpenGL unit 17
#define SLEAK_CLOUD_NOISE_SET 0
#ifndef SLEAK_CLOUD_NOISE_VK
#define SLEAK_CLOUD_NOISE_VK 5
#endif
#define SLEAK_CLOUD_NOISE_GL 17
layout(SLEAK_BINDING(SLEAK_CLOUD_NOISE_SET, SLEAK_CLOUD_NOISE_VK,
                     SLEAK_CLOUD_NOISE_GL)) uniform sampler2D uCloudNoise;

// Slab thickness and the world size of one noise tile, metres
#define SLEAK_CLOUD_THICKNESS 60.0
#define SLEAK_CLOUD_PERIOD 12000.0
// Samples per crossing; a full crossing samples the same world heights from
// every pixel, so slices stay still without dither or history
#define SLEAK_CLOUD_SAMPLES 16
// Opacity per sample is authored for this many samples per crossing
#define SLEAK_CLOUD_SAMPLES_REF 10.0
// Horizontal distance where clouds start thinning and are gone
#define SLEAK_CLOUD_FADE_START 250.0
#define SLEAK_CLOUD_FADE_END 1800.0
// Density: gain * (noise - threshold) - profile, soft clipped
#define SLEAK_CLOUD_EXCESS_GAIN 20.0
#define SLEAK_CLOUD_DENSITY 0.5
#define SLEAK_CLOUD_SOFT_CLIP 0.5
// Height profile gain * ((h - core) * slope)^2: steep below the core height
// (flat base), gentle above it (rounded tops)
#define SLEAK_CLOUD_CORE 0.125
#define SLEAK_CLOUD_SLOPE_BELOW 8.0
#define SLEAK_CLOUD_SLOPE_ABOVE 1.14
#define SLEAK_CLOUD_PROFILE_GAIN 4.0
// Hole around the light: depth * sat(2 cos - 1)^power off the density
#define SLEAK_CLOUD_REVEAL_DEPTH 3.0
#define SLEAK_CLOUD_REVEAL_POWER 12.0
// Sample light (height * h^(gain s^2 + base) + floor) * (1 - d^kappa),
// s the forward-scatter shape, kappa (base - sun VoL) * scale
#define SLEAK_CLOUD_LIGHT_HEIGHT 0.8
#define SLEAK_CLOUD_LIGHT_FLOOR 0.2
#define SLEAK_CLOUD_HEIGHT_POWER_GAIN 1.125
#define SLEAK_CLOUD_HEIGHT_POWER_BASE 0.875
#define SLEAK_CLOUD_KAPPA_BASE 2.0
#define SLEAK_CLOUD_KAPPA_SUN 1.5
#define SLEAK_CLOUD_KAPPA_SCALE 2.0
// Forward scatter s^6; |VoL| weight while the light flips sun to moon
#define SLEAK_CLOUD_PHASE_POWER 6.0
#define SLEAK_CLOUD_FLIP_SYMMETRY 0.8
// Silver lining on thin edges and the phase gain of the direct light
#define SLEAK_CLOUD_LINING 0.5
#define SLEAK_CLOUD_LIGHT_BASE 0.85
#define SLEAK_CLOUD_LIGHT_PHASE 1.15
// Stop marching once the ray is this opaque
#define SLEAK_CLOUD_OPAQUE 0.99
// maxDist of a sky ray: no surface stops it
#define SLEAK_CLOUD_OPEN_SKY 1e30

/// True when the cloud layer draws this frame.
bool SleakCloudsEnabled() { return uAtmosphere.CloudAmbient.w > 0.0; }

/// Cloud density in [0, 1) at a world xz and a height in the slab (0 base,
/// 1 top), less the hole around the light.
float SleakCloudDensity(vec2 xz, float height, float reveal) {
    vec2 drift = uAtmosphere.CloudLayer.zw * uFrame.Time;
    float noise =
        textureLod(uCloudNoise, (xz - drift) / SLEAK_CLOUD_PERIOD, 0.0).r;
    float slope = height > SLEAK_CLOUD_CORE ? SLEAK_CLOUD_SLOPE_ABOVE
                                            : SLEAK_CLOUD_SLOPE_BELOW;
    float k = (height - SLEAK_CLOUD_CORE) * slope;
    float n = SLEAK_CLOUD_EXCESS_GAIN * (noise - uAtmosphere.CloudLayer.y) -
              SLEAK_CLOUD_PROFILE_GAIN * k * k - reveal;
    n = max(n, 0.0) * SLEAK_CLOUD_DENSITY;
    return n * inversesqrt(n * n + SLEAK_CLOUD_SOFT_CLIP);
}

/// The cloud layer between the camera and maxDist along a unit direction:
/// rgb inscatter, a transmittance.
vec4 SleakAtmosphereClouds(vec3 dir, float maxDist) {
    vec3 cam = uFrame.CameraPos.xyz;
    float base = uAtmosphere.CloudLayer.x;
    float rise =
        abs(dir.y) > SLEAK_ATMO_EPSILON ? dir.y : SLEAK_ATMO_EPSILON;
    float tBase = (base - cam.y) / rise;
    float tTop = (base + SLEAK_CLOUD_THICKNESS - cam.y) / rise;
    float across = max(length(dir.xz), SLEAK_ATMO_EPSILON);
    float t0 = max(min(tBase, tTop), 0.0);
    float t1 = min(max(tBase, tTop),
                   min(maxDist, SLEAK_CLOUD_FADE_END / across));
    if (t1 <= t0) return vec4(0.0, 0.0, 0.0, 1.0);

    // Per ray: forward scatter, density darkening and the light's hole
    float VoL = dot(dir, uAtmosphere.GlareDir.xyz);
    float flip = uAtmosphere.CloudLight.w;
    float s = SLEAK_ATMO_HALF +
              SLEAK_ATMO_HALF *
                  mix(SLEAK_CLOUD_FLIP_SYMMETRY * abs(VoL), VoL, flip);
    float phase = pow(s, SLEAK_CLOUD_PHASE_POWER);
    float heightPower =
        SLEAK_CLOUD_HEIGHT_POWER_GAIN * s * s + SLEAK_CLOUD_HEIGHT_POWER_BASE;
    float kappa =
        (SLEAK_CLOUD_KAPPA_BASE - SLEAK_CLOUD_KAPPA_SUN * VoL * flip) *
        SLEAK_CLOUD_KAPPA_SCALE;
    float toLight = mix(abs(VoL), max(VoL, 0.0), flip);
    float reveal =
        SLEAK_CLOUD_REVEAL_DEPTH *
        pow(clamp(2.0 * toLight - 1.0, 0.0, 1.0), SLEAK_CLOUD_REVEAL_POWER);

    float spacing = (t1 - t0) / float(SLEAK_CLOUD_SAMPLES);
    float perSample = SLEAK_CLOUD_SAMPLES_REF / float(SLEAK_CLOUD_SAMPLES);
    float fadeRange = SLEAK_CLOUD_FADE_END - SLEAK_CLOUD_FADE_START;
    float alpha = 0.0;
    float light = 0.0;
    for (int i = 0; i < SLEAK_CLOUD_SAMPLES; ++i) {
        float t = t0 + (float(i) + SLEAK_ATMO_HALF) * spacing;
        vec3 p = cam + dir * t;
        float height = clamp((p.y - base) / SLEAK_CLOUD_THICKNESS, 0.0, 1.0);
        float fade =
            clamp((SLEAK_CLOUD_FADE_END - t * across) / fadeRange, 0.0, 1.0);
        float d = SleakCloudDensity(p.xz, height, reveal) * fade;
        float a = d * perSample;
        float lit = (SLEAK_CLOUD_LIGHT_HEIGHT * pow(height, heightPower) +
                     SLEAK_CLOUD_LIGHT_FLOOR) *
                    (1.0 - pow(d, kappa));
        light = mix(light, lit, a * (1.0 - alpha * alpha));
        alpha = mix(alpha, 1.0, a);
        if (alpha > SLEAK_CLOUD_OPAQUE) break;
    }
    light =
        mix(light, 1.0, (1.0 - alpha * alpha) * phase * SLEAK_CLOUD_LINING);
    vec3 direct = uAtmosphere.CloudLight.rgb *
                  (SLEAK_CLOUD_LIGHT_BASE + SLEAK_CLOUD_LIGHT_PHASE * phase);
    vec3 color = mix(uAtmosphere.CloudAmbient.rgb, direct, light);
    float cover = alpha * alpha;
    return vec4(color * cover, 1.0 - cover);
}

/// Everything a sky pixel shows toward a direction, clouds included; the
/// glare stays in front of them. Every sky draw uses it.
vec3 SleakAtmosphereSkyPixel(vec3 dir) {
    if (!SleakCloudsEnabled()) return SleakAtmosphereSky(dir);
    vec4 c = SleakAtmosphereClouds(dir, SLEAK_CLOUD_OPEN_SKY);
    return SleakAtmosphereSkyBehind(dir) * c.a + c.rgb +
           SleakAtmosphereGlare(dir);
}

/// The sky probe's view toward a direction: the gradient behind clouds.
vec3 SleakAtmosphereProbeSky(vec3 dir) {
    vec3 sky = SleakAtmosphereSkyGradient(dir);
    if (!SleakCloudsEnabled()) return sky;
    vec4 c = SleakAtmosphereClouds(dir, SLEAK_CLOUD_OPEN_SKY);
    return sky * c.a + c.rgb;
}

/// Fog and clouds between the camera and a point rel from it.
vec4 SleakAtmosphereCloudsOver(vec4 scatter, vec3 rel) {
    float dist = length(rel);
    vec4 c = SleakAtmosphereClouds(rel / max(dist, SLEAK_ATMO_EPSILON), dist);
    return vec4(scatter.rgb * c.a + c.rgb, scatter.a * c.a);
}

#endif
