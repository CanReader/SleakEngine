#version 450 core

// OpenGL deferred lighting pass. Reads the GBuffer at units 8-11, rebuilds
// world position from depth, lights it with every active light in LightUBO
// (PCF shadows for directional lights), adds hemisphere ambient plus IBL,
// then applies ACES and fog since GL has no post chain.

in  vec2 fragUV;
out vec4 outColor;

// GBuffer samplers (bound at texture units 8-11)
layout(binding = 8)  uniform sampler2D gbAlbedoAO;
layout(binding = 9)  uniform sampler2D gbNormalRough;
layout(binding = 10) uniform sampler2D gbMetalEmit;
layout(binding = 11) uniform sampler2D gbDepth;  // world pos reconstructed from this

// SSAO (bound at unit 13; blurred R8 AO, 1.0 means fully lit)
layout(binding = 13) uniform sampler2D gbSSAO;
uniform int uSSAOEnabled;

// IBL (bound at units 14-16 by EnsureIBL/Bind)
layout(binding = 14) uniform samplerCube irradianceMap;
layout(binding = 15) uniform samplerCube prefilterMap;
layout(binding = 16) uniform sampler2D   brdfLUT;

// Shadow map (already bound at unit 3 by the shadow pass)
layout(binding = 3) uniform sampler2DShadow shadowMap;

// ---- UBOs ----
struct LightData {
    vec3  Position;  uint Type;
    vec3  Direction; float Intensity;
    vec3  Color;     float Range;
    float SpotInnerCos; float SpotOuterCos;
    float AreaWidth;    float AreaHeight;
};

layout(std140, binding = 2) uniform LightUBO {
    vec3  CameraPos;
    uint  NumActiveLights;
    vec3  AmbientColor;
    float AmbientIntensity;
    vec4  FogColor;            // horizon
    float FogStart;
    float FogEnd;
    float _lightPad0, _lightPad1;
    LightData Lights[16];
    // Trailing block, appended after Lights[] so older shader copies that
    // stop here still read a valid prefix. Keep field order in sync with
    // LightCBData in ConstantBuffer.hpp.
    vec4  FogColorZenith;      // zenith
    float HeightFogTop;
    float HeightFogDensity;
    float HeightFogFalloff;
    float HeightFogEnabled;
};

layout(std140, binding = 5) uniform ShadowUBO {
    mat4  ShadowLightVP;
    float ShadowBias;
    float ShadowStrength;
    float ShadowTexelSize;
    float ShadowLightSize;
    uint  PCSSEnabled;
    uint  ShadowMapEnabled;
    float _shadowPad0, _shadowPad1;
    mat4  NdcToShadow;   // NDC -> shadow clip, CPU-composed (no shimmer)
};

layout(std140, binding = 6) uniform DeferredCB {
    mat4  InvViewProj;
    float ScreenWidth;
    float ScreenHeight;
    float NearPlane;
    float FarPlane;
};

layout(std140, binding = 10) uniform IBLUBO {
    uint  IBLEnabled;
    float IBLIntensity;
    float MaxReflectionLOD;
    uint  _iblPad0;
};

// ---- Constants ----
const float PI = 3.14159265359;

// Poisson disk for PCF
const vec2 poissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
);

// ---- PBR helper functions ----
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom  = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-7);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    float oneMinusRoughness = 1.0 - roughness;
    return F0 + (max(vec3(oneMinusRoughness), F0) - F0)
             * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---- Shadow ----
// World-space projection via the texel-snapped ShadowLightVP (the
// NdcToShadow composition shimmered under camera motion).
float CalcShadowPCF(vec3 worldPos, float NdotL) {
    if (ShadowMapEnabled == 0u) return 1.0;

    vec4 sc = ShadowLightVP * vec4(worldPos, 1.0);
    vec3 proj = sc.xyz / sc.w;
    proj.xy   = proj.xy * 0.5 + 0.5;
    proj.z    = proj.z  * 0.5 + 0.5;   // remap [-1,1]->  [0,1] for OpenGL

    if (proj.z > 1.0 || any(lessThan(proj.xy, vec2(0.0))) || any(greaterThan(proj.xy, vec2(1.0))))
        return 1.0;

    vec2 fade = smoothstep(vec2(0.0), vec2(0.05), proj.xy)
              * smoothstep(vec2(0.0), vec2(0.05), 1.0 - proj.xy);
    float edgeFade = fade.x * fade.y;

    float shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        shadow += texture(shadowMap,
            vec3(proj.xy + poissonDisk[i] * ShadowTexelSize * 5.0,
                 proj.z - ShadowBias));
    }
    shadow /= 16.0;

    // Fade shadow on surfaces facing away from light
    float fadedShadow = shadow * smoothstep(0.0, 0.15, NdotL);
    return mix(1.0, fadedShadow, ShadowStrength * edgeFade);
}

// Reconstruct world position from depth. OpenGL: no vertex Y-flip, and the
// default clip depth range is [-1,1], so the stored [0,1] depth maps to
// NDC.z = 2*depth-1 before InvViewProj.
vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc   = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = InvViewProj * ndc;
    return world.xyz / world.w;
}

// ---- ACES tone mapping ----
vec3 ACESFilm(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// Sky-matched gradient fog + exponential height fog. View direction picks a
// blend between horizon and zenith colors so distant geometry dissolves into
// the same color the sky renders behind it.
vec3 ApplyFog(vec3 color, vec3 worldPos) {
    if (FogEnd <= 0.0) return color;
    vec3 toFrag = worldPos - CameraPos;
    float dist = length(toFrag.xz);
    float distFog = clamp((dist - FogStart) / max(FogEnd - FogStart, 1e-4), 0.0, 1.0);
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

void main() {
    // ---- Sample GBuffer ----
    vec4  albedoAO     = texture(gbAlbedoAO,    fragUV);
    vec4  normalRough  = texture(gbNormalRough,  fragUV);
    vec4  metalEmit    = texture(gbMetalEmit,    fragUV);
    float depth        = texture(gbDepth,         fragUV).r;

    // Sky pixel, skip lighting (also leaves depth=1.0 for skybox pass)
    if (depth >= 1.0) discard;

    // Write GBuffer depth to the default FBO depth buffer so that the
    // forward transparent pass (skybox, water) has correct depth for testing.
    gl_FragDepth = depth;

    vec3  albedo    = albedoAO.rgb;
    float ao        = albedoAO.a;
    // Multiply per-vertex AO by screen-space AO. Remap SSAO result from
    // [0,1] to [0.5,1] so it can at most halve ambient. Prevents shadowed
    // surfaces from collapsing to pitch black when the shadow map has
    // already zeroed out direct light.
    if (uSSAOEnabled != 0) {
        float ssao = texture(gbSSAO, fragUV).r;
        ao *= 0.5 + 0.5 * ssao;
    }
    vec3  N         = normalize(normalRough.rgb * 2.0 - 1.0);  // unpack from [0,1]
    float roughness = normalRough.a;
    float metallic  = metalEmit.r;
    float emitScale = metalEmit.g;

    // World position reconstructed from depth + InvViewProj (no worldpos RT).
    vec3 worldPos = ReconstructWorldPos(fragUV, depth);
    vec4 ndcPos   = vec4(fragUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);

    // ---- Lighting accumulation (matches forward shader style) ----
    vec3 totalDiffuse = vec3(0.0);

    for (uint i = 0u; i < NumActiveLights; ++i) {
        LightData light = Lights[i];

        vec3 L;
        float attenuation = 1.0;

        if (light.Type == 0u) {
            // Directional
            L = normalize(-light.Direction);
        } else if (light.Type == 1u) {
            // Point
            vec3 toLight = light.Position - worldPos;
            float dist   = length(toLight);
            L            = toLight / dist;
            attenuation  = 1.0 / (1.0 + dist * dist / max(light.Range * light.Range, 1e-4));
        } else {
            continue;
        }

        // Wrap diffuse (matches forward shader 0.85 wrap)
        float rawNdotL = dot(N, L);
        float NdotL    = clamp(rawNdotL * 0.85 + 0.15, 0.0, 1.0);

        float shadow = 1.0;
        if (light.Type == 0u) {
            shadow = CalcShadowPCF(worldPos, max(rawNdotL, 0.0));
            // Fade shadow for back-facing surfaces (prevents light leaking through)
            shadow *= smoothstep(0.0, 0.15, rawNdotL);
        }

        totalDiffuse += light.Color * light.Intensity * attenuation * NdotL * shadow;
    }

    // ---- Ambient + AO ----
    // Hemisphere ambient is the base. With IBL enabled we ADD reflected
    // environmental light on top. The additive form keeps the scene from going
    // dim when the source skybox is LDR (low irradiance dynamic range).
    vec3  V             = normalize(CameraPos - worldPos);
    vec3  F0            = mix(vec3(0.04), albedo, metallic);
    vec3  skyAmbient    = AmbientColor * AmbientIntensity;
    vec3  groundAmbient = skyAmbient * vec3(0.55, 0.50, 0.45);
    float hemisphere    = N.y * 0.5 + 0.5;
    vec3  hemAmbient    = mix(groundAmbient, skyAmbient, hemisphere);
    vec3  ambient       = albedo * hemAmbient;

    if (IBLEnabled != 0u) {
        float NdotV_ibl       = max(dot(N, V), 0.0);
        vec3  kS_ibl          = FresnelSchlickRoughness(NdotV_ibl, F0, roughness);
        vec3  kD_ibl          = (1.0 - kS_ibl) * (1.0 - metallic);
        vec3  irradiance      = texture(irradianceMap, N).rgb;
        vec3  diffuseIBL      = irradiance * albedo;
        vec3  R               = reflect(-V, N);
        vec3  prefilteredColor= textureLod(prefilterMap, R,
                                            roughness * MaxReflectionLOD).rgb;
        vec2  envBRDF         = texture(brdfLUT,
                                        vec2(NdotV_ibl, roughness)).rg;
        vec3  specularIBL     = prefilteredColor
                              * (kS_ibl * envBRDF.x + envBRDF.y);
        ambient += (kD_ibl * diffuseIBL + specularIBL) * IBLIntensity;
    }
    ambient *= ao;

    // Diffuse term carries albedo here (totalDiffuse is light radiance only).
    vec3 color = ambient + albedo * totalDiffuse;

    // ---- Emissive ----
    color += albedo * emitScale;

    // ---- Tone mapping ----
    color = ACESFilm(color);

    // ---- Fog ----
    color = ApplyFog(color, worldPos);

    outColor = vec4(color, 1.0);
}
