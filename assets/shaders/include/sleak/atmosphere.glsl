#ifndef SLEAK_ATMOSPHERE_GLSL
#define SLEAK_ATMOSPHERE_GLSL

#include "sleak/binding.glsl"
#include "sleak/frame.glsl"

// ATMOSPHERE_DATA_VERSION in ConstantBuffer.hpp; bump both on every append
#define SLEAK_ATMOSPHERE_VERSION 2u

// Sky and fog, std140, append only (AtmosphereData in ConstantBuffer.hpp)
layout(std140, SLEAK_BINDING(SLEAK_FRAME_SET, SLEAK_ATMOSPHERE_VK,
                             SLEAK_ATMOSPHERE_GL)) uniform AtmosphereUBO {
    uvec4 Header;       // version, 1 while the atmosphere draws
    vec4  SunDir;       // xyz toward the sun, w disc intensity
    vec4  SunColor;     // rgb disc base color, w redden gain
    vec4  MoonDir;      // xyz toward the moon, w phase angle
    vec4  MoonTangent;  // xyz direction of travel at the moon
    vec4  MoonColor;    // rgb lit disc
    vec4  SkyColor;     // rgb day base before the soft clip, w day scale
    vec4  TintColor;    // rgb sun-side tint, w peak share
    vec4  NightColor;   // rgb night sky at the horizon, w day weight
    vec4  SkyShape;     // tint exponent, darkening below the horizon
    vec4  GlareDir;     // xyz toward the light, w lobe width
    vec4  GlareColor;   // rgb radiance at the lobe peak
    vec4  StarColor;    // rgb brightest star, w cosine of the sky turn
    vec4  StarAxis;     // xyz axis the sky turns around, w sine of the turn
    vec4  FogDensity;   // per metre, exponent, base y, halvings per metre
    vec4  FogIndoor;    // rgb enclosed fog color, w outdoor factor
    vec4  FogBorder;    // fog day weight, edge fade start and end
    vec4  CloudLayer;   // base y, noise threshold, wind x and z (m/s)
    vec4  CloudLight;   // rgb direct light on the clouds, w light fade
    vec4  CloudAmbient; // rgb sky light on the clouds, w coverage (0 off)
} uAtmosphere;

// Day gradient exp(-(1 - (1 - up)^shape) / density); the shape runs from
// across the sun to toward it
#define SLEAK_ATMO_SKY_DENSITY 0.35
#define SLEAK_ATMO_SKY_SHAPE_SIDE 1.5
#define SLEAK_ATMO_SKY_SHAPE_SUN 1.0
// Night gradient exp(-up / density)
#define SLEAK_ATMO_NIGHT_DENSITY 0.65
// Tint share along the horizon: band * (1 - |up|)^exponent
#define SLEAK_ATMO_HORIZON_BAND 0.125
#define SLEAK_ATMO_HORIZON_BAND_EXPONENT 2.5
// Ground darkening below the horizon: sat(-slope * up - offset)
#define SLEAK_ATMO_GROUND_SLOPE 1.015
#define SLEAK_ATMO_GROUND_OFFSET 0.015
// Disc sizes are 1 - cos of the edge; halos are strength * cos^exponent
#define SLEAK_ATMO_SUN_SIZE 0.007
#define SLEAK_ATMO_SUN_HALO 0.05
#define SLEAK_ATMO_SUN_HALO_EXPONENT 192.0
#define SLEAK_ATMO_MOON_SIZE 0.003
#define SLEAK_ATMO_MOON_HALO 0.01
#define SLEAK_ATMO_MOON_HALO_EXPONENT 256.0
// The sun reddens with power 1 + gain * (1 - up)^power
#define SLEAK_ATMO_SUN_REDDEN_POWER 16.0
// Half width of the moon's terminator on its sphere normal
#define SLEAK_ATMO_MOON_TERMINATOR 0.05
// Discs and halos sink: 1 - (1 - max(slope * up + offset, 0))^power
#define SLEAK_ATMO_DISC_FADE_SLOPE 0.98
#define SLEAK_ATMO_DISC_FADE_OFFSET 0.02
#define SLEAK_ATMO_HALO_FADE_SLOPE 0.93
#define SLEAK_ATMO_HALO_FADE_OFFSET 0.07
#define SLEAK_ATMO_FADE_POWER 8.0
// Disc edge: smoothstep(1 - size, 1, cos)^power
#define SLEAK_ATMO_DISC_EDGE_POWER 3.0
// Glare lobe: sat((f / (1 - (1 - f) x) - f) * gain / (1 - f) - offset)
#define SLEAK_ATMO_LOBE_GAIN 1.015
#define SLEAK_ATMO_LOBE_OFFSET 0.015
// Star grid per cube face, star chance of a face-centre cell and the
// brightness law 1 - (1 - u)^shape
#define SLEAK_ATMO_STAR_CELLS 820.0
#define SLEAK_ATMO_STAR_PROBABILITY 0.00128
#define SLEAK_ATMO_STAR_BRIGHTNESS_SHAPE (1.0 / 3.0)
// Stars fade with up^power and clear a ring around the moon (cosines)
#define SLEAK_ATMO_STAR_HORIZON_POWER 0.25
#define SLEAK_ATMO_STAR_MOON_OUTER 0.992
#define SLEAK_ATMO_STAR_MOON_INNER 0.997
// Solid angle of a cube-face cell falls as (1 + u^2 + v^2)^-power
#define SLEAK_ATMO_CELL_SOLID_ANGLE_POWER 1.5
// pcg3d (Jarzynski and Olano 2020)
#define SLEAK_ATMO_PCG_MUL 1664525u
#define SLEAK_ATMO_PCG_INC 1013904223u
#define SLEAK_ATMO_PCG_SHIFT 16u
// Hash to [0, 1): the top 24 bits
#define SLEAK_ATMO_HASH_DROP 8u
#define SLEAK_ATMO_HASH_SCALE (1.0 / 16777216.0)
// Fog T = exp(-strength * x^exponent), x the optical depth, whose slope
// drops beyond the knee
#define SLEAK_ATMO_FOG_STRENGTH 2.0
#define SLEAK_ATMO_FOG_KNEE 0.5
#define SLEAK_ATMO_FOG_KNEE_SLOPE 0.25
// Fog color gradients over the view height, and the distance over which
// the sun tint comes in
#define SLEAK_ATMO_FOG_GRADIENT_DAY 1.25
#define SLEAK_ATMO_FOG_GRADIENT_NIGHT 0.35
#define SLEAK_ATMO_FOG_TINT_DISTANCE 64.0
// Half: VoL and the fog view height map from [-1, 1] to [0, 1]
#define SLEAK_ATMO_HALF 0.5
// Keeps divisions finite
#define SLEAK_ATMO_EPSILON 1e-6
// Cube faces in Vulkan and OpenGL order
#define SLEAK_CUBE_FACE_POS_X 0u
#define SLEAK_CUBE_FACE_NEG_X 1u
#define SLEAK_CUBE_FACE_POS_Y 2u
#define SLEAK_CUBE_FACE_NEG_Y 3u
#define SLEAK_CUBE_FACE_POS_Z 4u
#define SLEAK_CUBE_FACE_NEG_Z 5u

/// True when the renderer draws the atmosphere this frame.
bool SleakAtmosphereEnabled() { return uAtmosphere.Header.y != 0u; }

/// True in Debug engine builds when this shader expects another block.
bool SleakAtmosphereStale() {
    return (uFrame.Flags.x & SLEAK_FRAME_FLAG_GUARD) != 0u &&
           SleakAtmosphereEnabled() &&
           uAtmosphere.Header.x != SLEAK_ATMOSPHERE_VERSION;
}

/// Per-channel soft clip c / sqrt(c^2 + 1).
vec3 SleakAtmoSoftClip(vec3 c) { return c * inversesqrt(c * c + 1.0); }

/// 0 below the horizon to 1 above it, the way a disc or halo sinks.
float SleakAtmoHorizonFade(float up, float slope, float offset) {
    float f = 1.0 - pow(1.0 - max(up * slope + offset, 0.0),
                        SLEAK_ATMO_FADE_POWER);
    return smoothstep(0.0, 1.0, f);
}

/// Pulls a sky or fog color toward the sun-side tint in square-root space.
vec3 SleakAtmoSunTint(vec3 base, float up, float toSun, float grad,
                      float weight) {
    float side = clamp(1.0 - up, 0.0, 1.0);
    float sunMix = pow((toSun * SLEAK_ATMO_HALF + SLEAK_ATMO_HALF) * side,
                       uAtmosphere.SkyShape.x) *
                   uAtmosphere.TintColor.w;
    float band = SLEAK_ATMO_HORIZON_BAND *
                 pow(1.0 - abs(up), SLEAK_ATMO_HORIZON_BAND_EXPONENT);
    float m = (1.0 - (1.0 - sunMix) * (1.0 - band)) * weight;
    vec3 tint = uAtmosphere.TintColor.rgb * grad;
    vec3 c = mix(sqrt(max(base * (1.0 - m), vec3(0.0))), sqrt(tint), m);
    return c * c;
}

/// Sky radiance toward a direction without discs, stars or glare.
vec3 SleakAtmosphereSkyGradient(vec3 dir) {
    float up = clamp(dir.y, -1.0, 1.0);
    float upper = max(up, 0.0);
    float toSun = clamp(dot(dir, uAtmosphere.SunDir.xyz), -1.0, 1.0);
    float shape = SLEAK_ATMO_SKY_SHAPE_SIDE +
                  (SLEAK_ATMO_SKY_SHAPE_SUN - SLEAK_ATMO_SKY_SHAPE_SIDE) *
                      toSun;
    float grad = exp(-(1.0 - pow(1.0 - upper, shape)) /
                     SLEAK_ATMO_SKY_DENSITY);
    vec3 day = SleakAtmoSoftClip(uAtmosphere.SkyColor.rgb * grad) *
               uAtmosphere.SkyColor.w;
    day = SleakAtmoSunTint(day, up, toSun, grad, 1.0);
    vec3 night = uAtmosphere.NightColor.rgb *
                 exp(-upper / SLEAK_ATMO_NIGHT_DENSITY);
    vec3 c = mix(night, day, uAtmosphere.NightColor.w);
    float below = clamp(-up * SLEAK_ATMO_GROUND_SLOPE -
                            SLEAK_ATMO_GROUND_OFFSET, 0.0, 1.0);
    if (below > 0.0) c *= 1.0 - exp(-uAtmosphere.SkyShape.y / below);
    return c;
}

/// Sun disc and its small halo, reddened toward the horizon.
vec3 SleakAtmosphereSun(vec3 dir, float up) {
    float cosSun = dot(dir, uAtmosphere.SunDir.xyz);
    if (uAtmosphere.SunDir.w <= 0.0 || cosSun <= 0.0) return vec3(0.0);
    float disc = pow(smoothstep(1.0 - SLEAK_ATMO_SUN_SIZE, 1.0, cosSun),
                     SLEAK_ATMO_DISC_EDGE_POWER);
    float halo = pow(cosSun, SLEAK_ATMO_SUN_HALO_EXPONENT) *
                 SLEAK_ATMO_SUN_HALO;
    float shine =
        disc * SleakAtmoHorizonFade(up, SLEAK_ATMO_DISC_FADE_SLOPE,
                                    SLEAK_ATMO_DISC_FADE_OFFSET) +
        halo * SleakAtmoHorizonFade(up, SLEAK_ATMO_HALO_FADE_SLOPE,
                                    SLEAK_ATMO_HALO_FADE_OFFSET);
    float power = 1.0 + uAtmosphere.SunColor.w *
                            pow(1.0 - max(up, 0.0),
                                SLEAK_ATMO_SUN_REDDEN_POWER);
    vec3 color = pow(max(uAtmosphere.SunColor.rgb, vec3(0.0)), vec3(power));
    return color * (uAtmosphere.SunDir.w * shine);
}

/// Moon disc with phase shading and its small halo.
vec3 SleakAtmosphereMoon(vec3 dir, float up) {
    float cosMoon = dot(dir, uAtmosphere.MoonDir.xyz);
    if (uAtmosphere.MoonColor.rgb == vec3(0.0) || cosMoon <= 0.0)
        return vec3(0.0);
    float disc = pow(smoothstep(1.0 - SLEAK_ATMO_MOON_SIZE, 1.0, cosMoon),
                     SLEAK_ATMO_DISC_EDGE_POWER);
    if (disc > 0.0) {
        // Sphere normal on the disc, lit from the phase direction
        vec3 tangent = uAtmosphere.MoonTangent.xyz;
        vec3 bitangent = cross(uAtmosphere.MoonDir.xyz, tangent);
        float radius =
            sqrt(SLEAK_ATMO_MOON_SIZE * (2.0 - SLEAK_ATMO_MOON_SIZE));
        vec2 p = vec2(dot(dir, tangent), dot(dir, bitangent)) / radius;
        vec3 n = vec3(p, sqrt(max(1.0 - dot(p, p), 0.0)));
        float phase = uAtmosphere.MoonDir.w;
        vec3 lit = vec3(sin(phase), 0.0, cos(phase));
        disc *= smoothstep(-SLEAK_ATMO_MOON_TERMINATOR,
                           SLEAK_ATMO_MOON_TERMINATOR, dot(n, lit));
    }
    float halo = pow(cosMoon, SLEAK_ATMO_MOON_HALO_EXPONENT) *
                 SLEAK_ATMO_MOON_HALO;
    float shine =
        disc * SleakAtmoHorizonFade(up, SLEAK_ATMO_DISC_FADE_SLOPE,
                                    SLEAK_ATMO_DISC_FADE_OFFSET) +
        halo * SleakAtmoHorizonFade(up, SLEAK_ATMO_HALO_FADE_SLOPE,
                                    SLEAK_ATMO_HALO_FADE_OFFSET);
    return uAtmosphere.MoonColor.rgb * shine;
}

/// Three 32-bit hashes of three integers (pcg3d).
uvec3 SleakAtmoHash(uvec3 v) {
    v = v * SLEAK_ATMO_PCG_MUL + SLEAK_ATMO_PCG_INC;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> SLEAK_ATMO_PCG_SHIFT;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

/// Stars on a cube-mapped sphere that turns with the sky.
vec3 SleakAtmosphereStars(vec3 dir, float up) {
    if (uAtmosphere.StarColor.rgb == vec3(0.0) || up <= 0.0)
        return vec3(0.0);
    // Rodrigues turn of the view direction onto the star sphere
    vec3 turnAxis = uAtmosphere.StarAxis.xyz;
    float cosTurn = uAtmosphere.StarColor.w;
    vec3 d = dir * cosTurn + cross(turnAxis, dir) * uAtmosphere.StarAxis.w +
             turnAxis * (dot(turnAxis, dir) * (1.0 - cosTurn));
    vec3 a = abs(d);
    uint axis = (a.x >= a.y && a.x >= a.z) ? 0u : (a.y >= a.z ? 1u : 2u);
    float major = axis == 0u ? d.x : (axis == 1u ? d.y : d.z);
    vec2 onFace = (axis == 0u ? d.yz : (axis == 1u ? d.xz : d.xy)) /
                  abs(major);
    vec2 cell = min(floor((onFace * SLEAK_ATMO_HALF + SLEAK_ATMO_HALF) *
                          SLEAK_ATMO_STAR_CELLS),
                    vec2(SLEAK_ATMO_STAR_CELLS - 1.0));
    vec2 centre =
        (cell + SLEAK_ATMO_HALF) / SLEAK_ATMO_STAR_CELLS * 2.0 - 1.0;
    float weight = pow(1.0 + dot(centre, centre),
                       -SLEAK_ATMO_CELL_SOLID_ANGLE_POWER);
    uint face = axis * 2u + (major < 0.0 ? 1u : 0u);
    uvec3 h = SleakAtmoHash(uvec3(uvec2(cell), face));
    vec3 r = vec3(h >> SLEAK_ATMO_HASH_DROP) * SLEAK_ATMO_HASH_SCALE;
    if (r.x >= SLEAK_ATMO_STAR_PROBABILITY * weight) return vec3(0.0);
    float brightness =
        1.0 - pow(1.0 - r.y, SLEAK_ATMO_STAR_BRIGHTNESS_SHAPE);
    float horizon = pow(up, SLEAK_ATMO_STAR_HORIZON_POWER);
    float moonClear = 1.0 - smoothstep(SLEAK_ATMO_STAR_MOON_OUTER,
                                       SLEAK_ATMO_STAR_MOON_INNER,
                                       dot(dir, uAtmosphere.MoonDir.xyz));
    return uAtmosphere.StarColor.rgb * (brightness * horizon * moonClear);
}

/// Forward-peaked halo around the sun or moon on sky pixels.
vec3 SleakAtmosphereGlare(vec3 dir) {
    if (uAtmosphere.GlareColor.rgb == vec3(0.0)) return vec3(0.0);
    float f = uAtmosphere.GlareDir.w;
    float x = dot(dir, uAtmosphere.GlareDir.xyz) * SLEAK_ATMO_HALF +
              SLEAK_ATMO_HALF;
    float lobe = (f / (1.0 - (1.0 - f) * x) - f) * SLEAK_ATMO_LOBE_GAIN /
                     (1.0 - f) -
                 SLEAK_ATMO_LOBE_OFFSET;
    return uAtmosphere.GlareColor.rgb * clamp(lobe, 0.0, 1.0);
}

/// Sky behind the cloud layer: gradient, discs and stars.
vec3 SleakAtmosphereSkyBehind(vec3 dir) {
    float up = clamp(dir.y, -1.0, 1.0);
    return SleakAtmosphereSkyGradient(dir) + SleakAtmosphereSun(dir, up) +
           SleakAtmosphereMoon(dir, up) + SleakAtmosphereStars(dir, up);
}

/// Everything a cloudless sky pixel shows toward a direction.
vec3 SleakAtmosphereSky(vec3 dir) {
    return SleakAtmosphereSkyBehind(dir) + SleakAtmosphereGlare(dir);
}

/// Color distant fog tends to toward a direction, before transmittance.
vec3 SleakAtmosphereFogColor(vec3 dir, float dist) {
    float up = clamp(dir.y, -1.0, 1.0);
    float toSun = clamp(dot(dir, uAtmosphere.SunDir.xyz), -1.0, 1.0);
    float height = up * SLEAK_ATMO_HALF + SLEAK_ATMO_HALF;
    float grad = exp(-height * SLEAK_ATMO_FOG_GRADIENT_DAY);
    vec3 fog = SleakAtmoSoftClip(uAtmosphere.SkyColor.rgb * grad) *
               uAtmosphere.SkyColor.w;
    float near = dist / SLEAK_ATMO_FOG_TINT_DISTANCE;
    fog = SleakAtmoSunTint(fog, up, toSun, grad, 1.0 - exp(-near * near));
    vec3 night = uAtmosphere.NightColor.rgb *
                 exp(-height * SLEAK_ATMO_FOG_GRADIENT_NIGHT);
    fog = mix(night, fog, uAtmosphere.FogBorder.x);
    float outdoor = uAtmosphere.FogIndoor.w;
    return mix(uAtmosphere.FogIndoor.rgb, fog * outdoor, outdoor);
}

/// Share of the light from a point that reaches the camera through fog.
float SleakAtmosphereFogTransmittance(float dist, float worldY) {
    float x = dist * uAtmosphere.FogDensity.x;
    if (x <= 0.0) return 1.0;
    x = min(x, SLEAK_ATMO_FOG_KNEE_SLOPE * (x - SLEAK_ATMO_FOG_KNEE) +
                   SLEAK_ATMO_FOG_KNEE);
    x *= exp2(-max(worldY - uAtmosphere.FogDensity.z, 0.0) *
              uAtmosphere.FogDensity.w);
    return exp(-SLEAK_ATMO_FOG_STRENGTH * pow(x, uAtmosphere.FogDensity.y));
}

/// Fog and edge fade between the camera and a point: rgb inscatter, a
/// transmittance. rel is the point minus the camera.
vec4 SleakAtmosphereScatter(vec3 rel, float worldY) {
    float dist = length(rel);
    vec3 dir = rel / max(dist, SLEAK_ATMO_EPSILON);
    float t = SleakAtmosphereFogTransmittance(dist, worldY);
    vec3 inscatter = vec3(0.0);
    if (t < 1.0) inscatter = SleakAtmosphereFogColor(dir, dist) * (1.0 - t);
    // Edge of the drawn world, by horizontal distance
    float start = uAtmosphere.FogBorder.y;
    float end = uAtmosphere.FogBorder.z;
    if (end > start) {
        float edge = clamp((length(rel.xz) - start) / (end - start), 0.0, 1.0);
        if (edge > 0.0) {
            inscatter = mix(inscatter, SleakAtmosphereSkyGradient(dir), edge);
            t *= 1.0 - edge;
        }
    }
    return vec4(inscatter, t);
}

/// World direction of a cube-map texel; uv is (s, t) on the face.
vec3 SleakCubeFaceDirection(uint face, vec2 uv) {
    vec2 c = uv * 2.0 - 1.0;
    vec3 d;
    if (face == SLEAK_CUBE_FACE_POS_X) d = vec3(1.0, -c.y, -c.x);
    else if (face == SLEAK_CUBE_FACE_NEG_X) d = vec3(-1.0, -c.y, c.x);
    else if (face == SLEAK_CUBE_FACE_POS_Y) d = vec3(c.x, 1.0, c.y);
    else if (face == SLEAK_CUBE_FACE_NEG_Y) d = vec3(c.x, -1.0, -c.y);
    else if (face == SLEAK_CUBE_FACE_POS_Z) d = vec3(c.x, -c.y, 1.0);
    else d = vec3(-c.x, -c.y, -1.0);
    return normalize(d);
}

#endif
