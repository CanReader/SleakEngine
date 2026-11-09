#ifndef SLEAK_LIGHTING_DEFAULT_GLSL
#define SLEAK_LIGHTING_DEFAULT_GLSL

#include "sleak/lighting_inputs.glsl"

// Engine deferred lighting per backend; game stems reuse SleakShadeDefault

const float SLEAK_PI = 3.14159265359;
const float SLEAK_TWO_PI = 6.283185;
// Dielectric reflectance at normal incidence
const vec3 SLEAK_DIELECTRIC_F0 = vec3(0.04);
// Ground bounce relative to the sky ambient
const vec3 SLEAK_GROUND_TINT = vec3(0.55, 0.50, 0.45);
// SSAO can at most halve the ambient term
const float SLEAK_SSAO_FLOOR = 0.5;
// PCF edge fades in shadow UV and depth
const float SLEAK_SHADOW_EDGE_FADE = 0.05;
const float SLEAK_SHADOW_FAR_FADE = 0.1;
// PCF radius in shadow texels
const float SLEAK_PCF_RADIUS_TEXELS = 5.0;
// N.L below which back faces get no direct light
const float SLEAK_SHADOW_FACE_FADE = 0.15;
// Keeps fog and light ranges away from a zero divisor
const float SLEAK_RANGE_EPSILON = 1e-4;

#ifdef VULKAN

// Interleaved gradient noise weights (Jimenez 2014)
const vec3 SLEAK_IGN_MAGIC = vec3(0.06711056, 0.00583715, 52.9829189);
// Wrap lighting: N.L * scale + offset
const float SLEAK_WRAP_SCALE = 0.85;
const float SLEAK_WRAP_OFFSET = 0.15;
// Keeps the BRDF denominator away from zero
const float SLEAK_BRDF_EPSILON = 0.0001;
// Upper HDR clamp that keeps NaNs out of the bloom chain
const float SLEAK_MAX_HDR = 1000.0;
const int SLEAK_PCF_SAMPLES = 16;
// r_i * (cos, sin)(i * 2.39996323), r_i = sqrt((i + 0.5) / 16)
const vec2 SLEAK_VOGEL16[SLEAK_PCF_SAMPLES] = vec2[](
    vec2( 0.176776692,  0.000000000), vec2(-0.225772178,  0.206825846),
    vec2( 0.034557982, -0.393771189), vec2( 0.284571416,  0.371172631),
    vec2(-0.522223197, -0.092373738), vec2( 0.494695337, -0.314684824),
    vec2(-0.165465304,  0.615525170), vec2(-0.315562004, -0.607594155),
    vec2( 0.684642316,  0.250029722), vec2(-0.712255912,  0.294009357),
    vec2( 0.343354186, -0.733728746), vec2( 0.253730458,  0.808931929),
    vec2(-0.764746789, -0.443184328), vec2( 0.897133617, -0.197234073),
    vec2(-0.547505567,  0.778773187), vec2(-0.126488309, -0.976089476));

/// Per-pixel rotation noise in [0,1).
float SleakInterleavedGradientNoise(vec2 screenPos) {
    return fract(SLEAK_IGN_MAGIC.z *
                 fract(dot(screenPos, SLEAK_IGN_MAGIC.xy)));
}

/// Rotated 16-tap Vogel PCF over the hardware compare sampler.
float SleakPCFFilter(vec2 uv, float zRef, float filterRadius, float phi) {
    vec2  cs     = vec2(cos(phi), sin(phi)) * filterRadius;
    float shadow = 0.0;
    for (int i = 0; i < SLEAK_PCF_SAMPLES; ++i) {
        vec2 d   = SLEAK_VOGEL16[i];
        vec2 off = vec2(d.x * cs.x - d.y * cs.y, d.x * cs.y + d.y * cs.x);
        shadow  += textureLod(gShadow, vec3(uv + off, zRef), 0.0);
    }
    return shadow / float(SLEAK_PCF_SAMPLES);
}

/// Sun visibility at a world position, 1 outside the shadow map.
float SleakCalcShadow(vec3 worldPos, vec3 N) {
    float normalBias  = uLightDir.w;
    vec4  sc          = uLightVP * vec4(worldPos + N * normalBias, 1.0);
    vec3  projCoords  = sc.xyz / sc.w;
    projCoords.xy     = projCoords.xy * 0.5 + 0.5;

    if (any(lessThan(projCoords, vec3(0.0))) ||
        any(greaterThan(projCoords, vec3(1.0)))) return 1.0;

    vec2 fadeCoord =
        smoothstep(vec2(0.0), vec2(SLEAK_SHADOW_EDGE_FADE), projCoords.xy) *
        smoothstep(vec2(0.0), vec2(SLEAK_SHADOW_EDGE_FADE),
                   vec2(1.0) - projCoords.xy);
    float zFade = smoothstep(0.0, SLEAK_SHADOW_EDGE_FADE, projCoords.z) *
                  smoothstep(0.0, SLEAK_SHADOW_FAR_FADE, 1.0 - projCoords.z);
    float edgeFade = fadeCoord.x * fadeCoord.y * zFade;

    float zRef = projCoords.z - uShadowBias;
    // Dither in shadow-texel space so it stays put under camera motion
    float phi = SleakInterleavedGradientNoise(projCoords.xy /
                                              uShadowTexelSize) *
                SLEAK_TWO_PI;

    float filterRadius = uShadowTexelSize * SLEAK_PCF_RADIUS_TEXELS;
    float shadow = SleakPCFFilter(projCoords.xy, zRef, filterRadius, phi);
    return mix(1.0, shadow, uShadowStrength * edgeFade);
}

/// GGX normal distribution.
float SleakDistributionGGX(vec3 N, vec3 H, float roughness) {
    float a    = roughness * roughness;
    float a2   = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
    return a2 / (SLEAK_PI * denom * denom);
}

/// Schlick-GGX geometry term for direct light.
float SleakGeometrySchlickGGX(float NdotX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotX / (NdotX * (1.0 - k) + k);
}

/// Smith geometry term for view and light.
float SleakGeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return SleakGeometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * SleakGeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

/// Fresnel-Schlick.
vec3 SleakFresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

/// Fresnel-Schlick with roughness, for ambient light.
vec3 SleakFresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
             * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

/// Distance fog on XZ combined with height fog, horizon to zenith tint.
vec3 SleakApplyFog(vec3 color, vec3 worldPos) {
    if (uFogEnd <= 0.0) return color;
    vec3  toFrag   = worldPos - uCameraPos.xyz;
    float dist     = length(toFrag.xz);
    float distFog  = clamp((dist - uFogStart) /
                           max(uFogEnd - uFogStart, SLEAK_RANGE_EPSILON),
                           0.0, 1.0);
    float heightFog = 0.0;
    if (uHeightFogEnabled > 0.5) {
        float h = max(uHeightFogTop - worldPos.y, 0.0);
        heightFog = clamp(uHeightFogDensity *
                              (1.0 - exp(-h * uHeightFogFalloff)),
                          0.0, 1.0);
    }
    float fogAmount = 1.0 - (1.0 - distFog) * (1.0 - heightFog);
    vec3  viewDir   = normalize(toFrag);
    float t         = smoothstep(0.0, 1.0,
                                 clamp(viewDir.y * 0.5 + 0.5, 0.0, 1.0));
    vec3  fogColor  = mix(uFogColor.rgb, uFogZenithColor.rgb, t);
    return mix(color, fogColor, fogAmount);
}

/// World position of a pixel; Vulkan NDC y runs opposite to the UV.
vec3 SleakReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc   = vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    vec4 world = InvViewProj * ndc;
    return world.xyz / world.w;
}

/// Engine lighting of one non-sky pixel: wrap diffuse sun with PCF shadow,
/// hemisphere ambient, extra lights, emissive, fog. Linear HDR.
vec3 SleakShadeDefault(ivec2 px, vec2 uv, float depth) {
    vec3  worldPos  = SleakReconstructWorldPos(uv, depth);
    vec4  albedoAO  = texelFetch(gAlbedoAO,    px, 0);
    vec4  normalRg  = texelFetch(gNormalRough, px, 0);
    vec4  metalEmit = texelFetch(gMetalEmit,   px, 0);

    vec3  albedo    = albedoAO.rgb;
    float bakedAO   = albedoAO.a;
    float ssao      = texture(gSSAO, uv).r;
    float ao        = bakedAO * ssao;
    vec3  N         = normalize(normalRg.rgb * 2.0 - 1.0);
    float roughness = normalRg.a;
    float metallic  = metalEmit.r;
    vec3  emissive  = metalEmit.gba;

    vec3  V     = normalize(uCameraPos.xyz - worldPos);
    float NdotV = max(dot(N, V), 0.0);

    vec3 F0 = mix(SLEAK_DIELECTRIC_F0, albedo, metallic);

    vec3 Lo = vec3(0.0);
    vec3 ambient;
    if (iblEnabled != 0u) {
        // Cook-Torrance sun
        vec3  L    = normalize(-uLightDir.xyz);
        vec3  H    = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);

        if (NdotL > 0.0) {
            float D = SleakDistributionGGX(N, H, roughness);
            float G = SleakGeometrySmith(N, V, L, roughness);
            vec3  F = SleakFresnelSchlick(max(dot(H, V), 0.0), F0);

            vec3 kS = F;
            vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

            vec3 specular = (D * G * F) /
                            (4.0 * NdotV * NdotL + SLEAK_BRDF_EPSILON);

            float shadow = SleakCalcShadow(worldPos, N);
            shadow *= smoothstep(0.0, SLEAK_SHADOW_FACE_FADE, dot(N, L));

            vec3 lightColor = uLightColor.rgb * uLightColor.a;
            Lo = (kD * albedo / SLEAK_PI + specular) * lightColor * NdotL *
                 shadow;
        }

        // Extra directional lights, no shadows
        for (uint li = 0u; li < uNumExtraLights; ++li) {
            vec3  Lx    = normalize(-uExtraDir[li].xyz);
            vec3  Hx    = normalize(V + Lx);
            float NdotLx = max(dot(N, Lx), 0.0);
            if (NdotLx > 0.0) {
                float Dx = SleakDistributionGGX(N, Hx, roughness);
                float Gx = SleakGeometrySmith(N, V, Lx, roughness);
                vec3  Fx = SleakFresnelSchlick(max(dot(Hx, V), 0.0), F0);
                vec3  kSx = Fx;
                vec3  kDx = (vec3(1.0) - kSx) * (1.0 - metallic);
                vec3  specx = (Dx * Gx * Fx) /
                              (4.0 * NdotV * NdotLx + SLEAK_BRDF_EPSILON);
                vec3  lightColorx = uExtraColor[li].rgb * uExtraColor[li].a;
                Lo += (kDx * albedo / SLEAK_PI + specx) * lightColorx * NdotLx;
            }
        }

        // Split-sum IBL
        vec3 F_amb  = SleakFresnelSchlickRoughness(NdotV, F0, roughness);
        vec3 kS_amb = F_amb;
        vec3 kD_amb = (1.0 - kS_amb) * (1.0 - metallic);

        vec3 irradiance  = texture(iblIrradiance, N).rgb;
        vec3 diffuse_ibl = kD_amb * irradiance * albedo;

        vec3 R                = reflect(-V, N);
        vec3 prefilteredColor =
            textureLod(iblPrefilter, R, roughness * maxReflectionLOD).rgb;
        vec2 brdf             = texture(iblBrdfLUT, vec2(NdotV, roughness)).rg;
        vec3 specular_ibl     = prefilteredColor * (F_amb * brdf.x + brdf.y);

        ambient = (diffuse_ibl + specular_ibl) * ao * iblIntensity;
    } else {
        // Wrap diffuse sun and hemisphere ambient
        vec3 skyAmbient    = uAmbient.rgb * uAmbient.a;
        vec3 groundAmbient = skyAmbient * SLEAK_GROUND_TINT;
        float hemisphere   = N.y * 0.5 + 0.5;
        vec3  Ldir    = normalize(-uLightDir.xyz);
        float rawNdL  = dot(N, Ldir);
        float wrapNdL =
            clamp(rawNdL * SLEAK_WRAP_SCALE + SLEAK_WRAP_OFFSET, 0.0, 1.0);
        float shad    = 0.0;
        if (rawNdL > -SLEAK_SHADOW_FACE_FADE)
            shad = SleakCalcShadow(worldPos, N) *
                   smoothstep(-SLEAK_SHADOW_FACE_FADE, 0.0, rawNdL);
        vec3 direct   = uLightColor.rgb * uLightColor.a * wrapNdL * shad;
        for (uint li = 0u; li < uNumExtraLights; ++li) {
            vec3  Lx   = normalize(-uExtraDir[li].xyz);
            float NdLx = max(dot(N, Lx), 0.0);
            direct += uExtraColor[li].rgb * uExtraColor[li].a * NdLx;
        }
        float aoGL = bakedAO * (SLEAK_SSAO_FLOOR + SLEAK_SSAO_FLOOR * ssao);
        ambient = albedo * mix(groundAmbient, skyAmbient, hemisphere) * aoGL;
        Lo      = albedo * direct;
    }

    vec3 color = ambient + Lo + emissive;

    // Fog in linear HDR; the composite pass tonemaps
    color = SleakApplyFog(color, worldPos);
    return clamp(color, vec3(0.0), vec3(SLEAK_MAX_HDR));
}

/// The whole engine lighting pass: sky stays black for the skybox.
void SleakLightingDefaultMain() {
    ivec2 px    = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(gDepth, px, 0).r;
    if (depth >= 1.0) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    outColor = vec4(SleakShadeDefault(px, fragUV, depth), 1.0);
}

#else  // OpenGL

// Poisson disk for PCF
const int SLEAK_PCF_SAMPLES = 16;
const vec2 SLEAK_POISSON16[SLEAK_PCF_SAMPLES] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
);
// Wrap lighting: N.L * scale + offset
const float SLEAK_WRAP_SCALE = 0.85;
const float SLEAK_WRAP_OFFSET = 0.15;
// Keeps PBR denominators away from zero
const float SLEAK_PBR_EPSILON = 1e-7;
// ACES filmic fit (Narkowicz 2015): x(ax + b) / (x(cx + d) + e)
const float SLEAK_ACES_A = 2.51;
const float SLEAK_ACES_B = 0.03;
const float SLEAK_ACES_C = 2.43;
const float SLEAK_ACES_D = 0.59;
const float SLEAK_ACES_E = 0.14;
// Light types (LightType in Light.hpp)
const uint SLEAK_LIGHT_DIRECTIONAL = 0u;
const uint SLEAK_LIGHT_POINT = 1u;

/// GGX normal distribution.
float SleakDistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom  = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(SLEAK_PI * denom * denom, SLEAK_PBR_EPSILON);
}

/// Schlick-GGX geometry term for direct light.
float SleakGeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, SLEAK_PBR_EPSILON);
}

/// Smith geometry term for view and light.
float SleakGeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return SleakGeometrySchlickGGX(NdotV, roughness) *
           SleakGeometrySchlickGGX(NdotL, roughness);
}

/// Fresnel-Schlick.
vec3 SleakFresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

/// Fresnel-Schlick with roughness, for ambient light.
vec3 SleakFresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    float oneMinusRoughness = 1.0 - roughness;
    return F0 + (max(vec3(oneMinusRoughness), F0) - F0)
             * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

/// Sun visibility at a world position through the texel-snapped light VP.
float SleakCalcShadowPCF(vec3 worldPos, float NdotL) {
    if (ShadowMapEnabled == 0u) return 1.0;

    vec4 sc = ShadowLightVP * vec4(worldPos, 1.0);
    vec3 proj = sc.xyz / sc.w;
    proj.xy   = proj.xy * 0.5 + 0.5;
    proj.z    = SleakNdcToDepth(proj.z);

    if (proj.z > 1.0 || any(lessThan(proj.xy, vec2(0.0))) ||
        any(greaterThan(proj.xy, vec2(1.0))))
        return 1.0;

    vec2 fade =
        smoothstep(vec2(0.0), vec2(SLEAK_SHADOW_EDGE_FADE), proj.xy) *
        smoothstep(vec2(0.0), vec2(SLEAK_SHADOW_EDGE_FADE), 1.0 - proj.xy);
    float edgeFade = fade.x * fade.y;

    float shadow = 0.0;
    for (int i = 0; i < SLEAK_PCF_SAMPLES; ++i) {
        shadow += texture(gShadow,
            vec3(proj.xy + SLEAK_POISSON16[i] * ShadowTexelSize *
                               SLEAK_PCF_RADIUS_TEXELS,
                 proj.z - ShadowBias));
    }
    shadow /= float(SLEAK_PCF_SAMPLES);

    // Less shadow on surfaces facing away from the light
    float fadedShadow = shadow * smoothstep(0.0, SLEAK_SHADOW_FACE_FADE, NdotL);
    return mix(1.0, fadedShadow, ShadowStrength * edgeFade);
}

/// World position of a GBuffer texel from its depth.
vec3 SleakReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc   = vec4(SleakUvToNdc(uv), SleakDepthToNdc(depth), 1.0);
    vec4 world = InvViewProj * ndc;
    return world.xyz / world.w;
}

/// ACES filmic curve (Narkowicz fit).
vec3 SleakACESFilm(vec3 x) {
    return clamp((x * (SLEAK_ACES_A * x + SLEAK_ACES_B)) /
                     (x * (SLEAK_ACES_C * x + SLEAK_ACES_D) + SLEAK_ACES_E),
                 0.0, 1.0);
}

/// Distance and height fog toward a horizon-to-zenith sky tint.
vec3 SleakApplyFog(vec3 color, vec3 worldPos) {
    if (FogEnd <= 0.0) return color;
    vec3 toFrag = worldPos - CameraPos;
    float dist = length(toFrag.xz);
    float distFog = clamp((dist - FogStart) /
                          max(FogEnd - FogStart, SLEAK_RANGE_EPSILON),
                          0.0, 1.0);
    float heightFog = 0.0;
    if (HeightFogEnabled > 0.5) {
        float h = max(HeightFogTop - worldPos.y, 0.0);
        heightFog = clamp(HeightFogDensity * (1.0 - exp(-h * HeightFogFalloff)),
                          0.0, 1.0);
    }
    float fogAmount = 1.0 - (1.0 - distFog) * (1.0 - heightFog);
    vec3 viewDir = normalize(toFrag);
    float t = smoothstep(0.0, 1.0, clamp(viewDir.y * 0.5 + 0.5, 0.0, 1.0));
    vec3 fogColor = mix(FogColor.rgb, FogColorZenith.rgb, t);
    return mix(color, fogColor, fogAmount);
}

/// Engine lighting of one non-sky pixel: every light in LightUBO (PCF for
/// directional), hemisphere ambient plus IBL, emissive, ACES, then fog.
vec3 SleakShadeDefault(ivec2 px, vec2 uv, float depth) {
    vec4  albedoAO     = texture(gAlbedoAO,    uv);
    vec4  normalRough  = texture(gNormalRough, uv);
    vec4  metalEmit    = texture(gMetalEmit,   uv);

    vec3  albedo    = albedoAO.rgb;
    float ao        = albedoAO.a;
    if (SleakSSAOEnabled()) {
        float ssao = texture(gSSAO, uv).r;
        ao *= SLEAK_SSAO_FLOOR + SLEAK_SSAO_FLOOR * ssao;
    }
    vec3  N         = normalize(normalRough.rgb * 2.0 - 1.0);
    float roughness = normalRough.a;
    float metallic  = metalEmit.r;
    float emitScale = metalEmit.g;

    vec3 worldPos = SleakReconstructWorldPos(uv, depth);

    vec3 totalDiffuse = vec3(0.0);
    for (uint i = 0u; i < NumActiveLights; ++i) {
        SleakLightData light = Lights[i];

        vec3 L;
        float attenuation = 1.0;

        if (light.Type == SLEAK_LIGHT_DIRECTIONAL) {
            L = normalize(-light.Direction);
        } else if (light.Type == SLEAK_LIGHT_POINT) {
            vec3 toLight = light.Position - worldPos;
            float dist   = length(toLight);
            L            = toLight / dist;
            attenuation  = 1.0 / (1.0 + dist * dist /
                                  max(light.Range * light.Range,
                                      SLEAK_RANGE_EPSILON));
        } else {
            continue;
        }

        float rawNdotL = dot(N, L);
        float NdotL    = clamp(rawNdotL * SLEAK_WRAP_SCALE + SLEAK_WRAP_OFFSET,
                               0.0, 1.0);

        float shadow = 1.0;
        if (light.Type == SLEAK_LIGHT_DIRECTIONAL) {
            shadow = SleakCalcShadowPCF(worldPos, max(rawNdotL, 0.0));
            shadow *= smoothstep(0.0, SLEAK_SHADOW_FACE_FADE, rawNdotL);
        }

        totalDiffuse +=
            light.Color * light.Intensity * attenuation * NdotL * shadow;
    }

    // Hemisphere ambient, plus reflected environment light with IBL
    vec3  V             = normalize(CameraPos - worldPos);
    vec3  F0            = mix(SLEAK_DIELECTRIC_F0, albedo, metallic);
    vec3  skyAmbient    = AmbientColor * AmbientIntensity;
    vec3  groundAmbient = skyAmbient * SLEAK_GROUND_TINT;
    float hemisphere    = N.y * 0.5 + 0.5;
    vec3  hemAmbient    = mix(groundAmbient, skyAmbient, hemisphere);
    vec3  ambient       = albedo * hemAmbient;

    if (iblEnabled != 0u) {
        float NdotV_ibl       = max(dot(N, V), 0.0);
        vec3  kS_ibl          =
            SleakFresnelSchlickRoughness(NdotV_ibl, F0, roughness);
        vec3  kD_ibl          = (1.0 - kS_ibl) * (1.0 - metallic);
        vec3  irradiance      = texture(iblIrradiance, N).rgb;
        vec3  diffuseIBL      = irradiance * albedo;
        vec3  R               = reflect(-V, N);
        vec3  prefilteredColor= textureLod(iblPrefilter, R,
                                            roughness * maxReflectionLOD).rgb;
        vec2  envBRDF         = texture(iblBrdfLUT,
                                        vec2(NdotV_ibl, roughness)).rg;
        vec3  specularIBL     = prefilteredColor
                              * (kS_ibl * envBRDF.x + envBRDF.y);
        ambient += (kD_ibl * diffuseIBL + specularIBL) * iblIntensity;
    }
    ambient *= ao;

    vec3 color = ambient + albedo * totalDiffuse;
    color += albedo * emitScale;
    color = SleakACESFilm(color);
    return SleakApplyFog(color, worldPos);
}

/// The whole engine lighting pass: sky is discarded so the clear color and
/// depth 1 stay for the skybox; lit pixels copy the GBuffer depth.
void SleakLightingDefaultMain() {
    float depth = texture(gDepth, fragUV).r;
    if (depth >= 1.0) discard;
    gl_FragDepth = depth;
    vec3 color = SleakShadeDefault(ivec2(gl_FragCoord.xy), fragUV, depth);
    outColor = SleakFrameGuard(vec4(color, 1.0));
}

#endif

#endif
