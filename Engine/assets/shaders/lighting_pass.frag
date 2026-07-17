#version 450

// ============================================================
// Deferred Lighting Pass — Cook-Torrance PBR BRDF + IBL
//
// Set 0: GBuffer samplers (binding 0-6) + shadow maps
// Set 1: DeferredCB UBO
// Set 2: ShadowLightUBO (directional light + shadow params + fog)
// Set 3: IBL (irradiance cubemap, prefiltered env map, BRDF LUT, settings)
// ============================================================

layout(location = 0) in vec2 fragUV;

// -- Set 0: GBuffer -------------------------------------------------
layout(set = 0, binding = 0) uniform sampler2D gAlbedoAO;       // albedo.rgb + AO.a
layout(set = 0, binding = 1) uniform sampler2D gNormalRough;    // normal.xyz (encoded) + roughness.a
layout(set = 0, binding = 2) uniform sampler2D gMetalEmit;      // metallic.r + emissive.gba
layout(set = 0, binding = 3) uniform sampler2D gDepth;          // world pos reconstructed from this
layout(set = 0, binding = 4) uniform sampler2DShadow gShadow;   // hardware PCF
layout(set = 0, binding = 5) uniform sampler2D gShadowRaw;      // PCSS blocker search
layout(set = 0, binding = 6) uniform sampler2D gSSAO;           // screen-space AO (R8, 1=clear, 0=occluded)

// -- Set 1: Deferred CB ---------------------------------------------
layout(set = 1, binding = 0) uniform DeferredCB {
    mat4  InvViewProj;
    float ScreenW;
    float ScreenH;
    float NearP;
    float FarP;
};

// -- Set 2: Shadow + Light UBO --------------------------------------
layout(set = 2, binding = 0) uniform ShadowLightUBO {
    vec4  uLightDir;         // xyz=direction FROM light, w=normalBias
    vec4  uLightColor;       // rgb=color, a=intensity
    vec4  uAmbient;          // rgb=sky color, a=intensity
    vec4  uCameraPos;        // xyz=worldPos, w=sceneClock
    mat4  uLightVP;
    float uShadowBias;
    float uShadowStrength;
    float uShadowTexelSize;
    float uLightSize;
    vec4  uFogColor;         // horizon
    float uFogStart;
    float uFogEnd;
    vec2  _fogPad;
    vec4  uFogZenithColor;   // zenith
    float uHeightFogTop;
    float uHeightFogDensity;
    float uHeightFogFalloff;
    float uHeightFogEnabled;
    // Extra fill/rim lights (no shadows)
    vec4  uExtraDir[3];      // xyz=direction FROM light, w=unused
    vec4  uExtraColor[3];    // rgb=color, a=intensity
    uint  uNumExtraLights;   // 0-3
    // no pad member: std140 aligns the mat4 to 320, matching the C++ struct
    // (a vec3 pad here pushed uNdcToShadow to 336 -> garbage shadow matrix)
    mat4  uNdcToShadow;      // NDC -> shadow clip, CPU-composed (no shimmer)
};

// -- Set 3: IBL -----------------------------------------------------
layout(set = 3, binding = 0) uniform samplerCube iblIrradiance;
layout(set = 3, binding = 1) uniform samplerCube iblPrefilter;
layout(set = 3, binding = 2) uniform sampler2D   iblBrdfLUT;
layout(set = 3, binding = 3) uniform IBLSettings {
    uint  iblEnabled;
    float iblIntensity;
    float maxReflectionLOD;
    float _iblPad;
};

layout(location = 0) out vec4 outColor;

// ==================================================================
// PCSS Shadow
// ==================================================================

float InterleavedGradientNoise(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

vec2 VogelDisk(int i, int count, float phi) {
    const float goldenAngle = 2.39996323;
    float r     = sqrt((float(i) + 0.5) / float(count));
    float theta = float(i) * goldenAngle + phi;
    return vec2(r * cos(theta), r * sin(theta));
}

// Fixed-radius rotated PCF — crisp shadows like a classic forward renderer,
// at a fraction of PCSS cost (no blocker search). Hardware comparison sampler
// (gShadow) does a 2x2 lerp per tap, so 12 rotated taps read smooth and crisp.
const int PCF_SAMPLES = 12;

float PCFFilter(vec2 uv, float zRef, float filterRadius, float phi) {
    float shadow = 0.0;
    for (int i = 0; i < PCF_SAMPLES; ++i) {
        vec2 off = VogelDisk(i, PCF_SAMPLES, phi) * filterRadius;
        shadow  += texture(gShadow, vec3(uv + off, zRef));
    }
    return shadow / float(PCF_SAMPLES);
}

// ndc: screen NDC + depth (w=1). Shadow coords go straight through the
// CPU-composed uNdcToShadow — never via reconstructed world position.
float CalcShadow(vec4 ndc, vec3 N) {
    float normalBias  = uLightDir.w;
    vec4  sc          = uNdcToShadow * ndc;
    vec3  projCoords  = sc.xyz / sc.w;
    projCoords       += (uLightVP * vec4(N * normalBias, 0.0)).xyz;
    projCoords.xy     = projCoords.xy * 0.5 + 0.5;

    if (any(lessThan(projCoords, vec3(0.0))) ||
        any(greaterThan(projCoords, vec3(1.0)))) return 1.0;

    vec2 fadeCoord = smoothstep(vec2(0.0), vec2(0.05), projCoords.xy)
                   * smoothstep(vec2(0.0), vec2(0.05), vec2(1.0) - projCoords.xy);
    float zFade    = smoothstep(0.0, 0.05, projCoords.z)
                   * smoothstep(0.0, 0.1,  1.0 - projCoords.z);
    float edgeFade = fadeCoord.x * fadeCoord.y * zFade;

    float zRef = projCoords.z - uShadowBias;
    float phi  = InterleavedGradientNoise(gl_FragCoord.xy) * 6.283185;

    // Small fixed penumbra — crisp. ~2 texels of softening hides aliasing.
    float filterRadius = uShadowTexelSize * 2.0;
    float shadow = PCFFilter(projCoords.xy, zRef, filterRadius, phi);
    return mix(1.0, shadow, uShadowStrength * edgeFade);
}

// ==================================================================
// Cook-Torrance PBR BRDF
// ==================================================================

const float PI = 3.14159265359;

// GGX Normal Distribution Function (Trowbridge-Reitz)
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a    = roughness * roughness;
    float a2   = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

// Schlick-GGX geometry term with k for DIRECT lighting ((r+1)^2/8)
float GeometrySchlickGGX(float NdotX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotX / (NdotX * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

// Fresnel-Schlick
vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// Fresnel-Schlick roughness variant for IBL ambient
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
             * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// ==================================================================
// Fog
// ==================================================================
vec3 ApplyFog(vec3 color, vec3 worldPos) {
    if (uFogEnd <= 0.0) return color;
    vec3  toFrag   = worldPos - uCameraPos.xyz;
    float dist     = length(toFrag.xz);
    float distFog  = clamp((dist - uFogStart) / max(uFogEnd - uFogStart, 1e-4), 0.0, 1.0);
    float heightFog = 0.0;
    if (uHeightFogEnabled > 0.5) {
        float h = max(uHeightFogTop - worldPos.y, 0.0);
        heightFog = clamp(uHeightFogDensity * (1.0 - exp(-h * uHeightFogFalloff)), 0.0, 1.0);
    }
    float fogAmount = 1.0 - (1.0 - distFog) * (1.0 - heightFog);
    vec3  viewDir   = normalize(toFrag);
    float t         = smoothstep(0.0, 1.0, clamp(viewDir.y * 0.5 + 0.5, 0.0, 1.0));
    vec3  fogColor  = mix(uFogColor.rgb, uFogZenithColor.rgb, t);
    return mix(color, fogColor, fogAmount);
}

// ACES film tone mapping
vec3 ACESFilm(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 vec3(0.0), vec3(1.0));
}

// Reconstruct world position from the depth buffer.
// Vulkan: geometry is Y-flipped in the GBuffer vertex shader and depth is in
// [0,1], so NDC.y is negated relative to the sampling UV.
vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc   = vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    vec4 world = InvViewProj * ndc;
    return world.xyz / world.w;
}

// ==================================================================
// Main
// ==================================================================
void main() {
    // Early-out for sky pixels (depth == 1.0 means nothing was drawn)
    float depth = texture(gDepth, fragUV).r;
    if (depth >= 1.0) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    // Reconstruct world position from depth instead of a GBuffer RT.
    vec3  worldPos  = ReconstructWorldPos(fragUV, depth);
    vec4  ndcPos    = vec4(fragUV.x * 2.0 - 1.0, 1.0 - 2.0 * fragUV.y,
                           depth, 1.0);
    vec4  albedoAO  = texture(gAlbedoAO,    fragUV);
    vec4  normalRg  = texture(gNormalRough, fragUV);
    vec4  metalEmit = texture(gMetalEmit,   fragUV); // R16G16B16A16: .r=metallic, .gba=emissive

    vec3  albedo    = albedoAO.rgb;
    float bakedAO   = albedoAO.a;          // vertex AO baked into GBuffer — always [0.4, 1.0] for voxels
    float ssao      = texture(gSSAO, fragUV).r;
    float ao        = bakedAO * ssao;      // combined AO used for IBL path
    vec3  N         = normalize(normalRg.rgb * 2.0 - 1.0);
    float roughness = normalRg.a;
    float metallic  = metalEmit.r;
    vec3  emissive  = metalEmit.gba;  // HDR emissive colour

    vec3  V     = normalize(uCameraPos.xyz - worldPos);
    float NdotV = max(dot(N, V), 0.0);

    // F0: reflectance at normal incidence
    // Dielectrics use 0.04, metals use their albedo colour as F0
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // ------------------------------------------------------------------
    // Direct lighting: single directional light (Cook-Torrance BRDF)
    // ------------------------------------------------------------------
    vec3 Lo = vec3(0.0);
    {
        vec3  L    = normalize(-uLightDir.xyz);
        vec3  H    = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);

        if (NdotL > 0.0) {
            float D = DistributionGGX(N, H, roughness);
            float G = GeometrySmith(N, V, L, roughness);
            vec3  F = FresnelSchlick(max(dot(H, V), 0.0), F0);

            vec3 kS = F;
            vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);  // metals have no diffuse

            vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);

            float shadow = CalcShadow(ndcPos, N);
            // Suppress shadow on back-facing surfaces (prevents light leaks)
            shadow *= smoothstep(0.0, 0.15, dot(N, L));

            vec3 lightColor = uLightColor.rgb * uLightColor.a;
            Lo = (kD * albedo / PI + specular) * lightColor * NdotL * shadow;
        }
    }

    // ------------------------------------------------------------------
    // Extra directional lights (fill, rim — no shadows)
    // ------------------------------------------------------------------
    for (uint li = 0u; li < uNumExtraLights; ++li) {
        vec3  Lx    = normalize(-uExtraDir[li].xyz);
        vec3  Hx    = normalize(V + Lx);
        float NdotLx = max(dot(N, Lx), 0.0);
        if (NdotLx > 0.0) {
            float Dx = DistributionGGX(N, Hx, roughness);
            float Gx = GeometrySmith(N, V, Lx, roughness);
            vec3  Fx = FresnelSchlick(max(dot(Hx, V), 0.0), F0);
            vec3  kSx = Fx;
            vec3  kDx = (vec3(1.0) - kSx) * (1.0 - metallic);
            vec3  specx = (Dx * Gx * Fx) / (4.0 * NdotV * NdotLx + 0.0001);
            vec3  lightColorx = uExtraColor[li].rgb * uExtraColor[li].a;
            Lo += (kDx * albedo / PI + specx) * lightColorx * NdotLx;
        }
    }

    // ------------------------------------------------------------------
    // Indirect lighting: IBL split-sum approximation
    //   or hemisphere ambient fallback when IBL is not ready
    // ------------------------------------------------------------------
    vec3 ambient;
    if (iblEnabled != 0u) {
        vec3 F_amb  = FresnelSchlickRoughness(NdotV, F0, roughness);
        vec3 kS_amb = F_amb;
        vec3 kD_amb = (1.0 - kS_amb) * (1.0 - metallic);

        // Diffuse irradiance (pre-integrated cosine-weighted hemisphere)
        vec3 irradiance  = texture(iblIrradiance, N).rgb;
        vec3 diffuse_ibl = kD_amb * irradiance * albedo;

        // Specular split-sum: prefiltered env-map + BRDF integration LUT
        vec3 R                = reflect(-V, N);
        vec3 prefilteredColor = textureLod(iblPrefilter, R, roughness * maxReflectionLOD).rgb;
        vec2 brdf             = texture(iblBrdfLUT, vec2(NdotV, roughness)).rg;
        vec3 specular_ibl     = prefilteredColor * (F_amb * brdf.x + brdf.y);

        ambient = (diffuse_ibl + specular_ibl) * ao * iblIntensity;
    } else {
        // Classic voxel lighting: wrap diffuse + hemisphere ambient
        vec3 skyAmbient    = uAmbient.rgb * uAmbient.a;
        vec3 groundAmbient = skyAmbient * vec3(0.55, 0.50, 0.45);
        float hemisphere   = N.y * 0.5 + 0.5;
        vec3  Ldir    = normalize(-uLightDir.xyz);
        float rawNdL  = dot(N, Ldir);
        float wrapNdL = clamp(rawNdL * 0.85 + 0.15, 0.0, 1.0);
        float shad    = CalcShadow(ndcPos, N);
        shad         *= smoothstep(-0.15, 0.0, rawNdL);
        vec3 direct   = uLightColor.rgb * uLightColor.a * wrapNdL * shad;
        for (uint li = 0u; li < uNumExtraLights; ++li) {
            vec3  Lx   = normalize(-uExtraDir[li].xyz);
            float NdLx = max(dot(N, Lx), 0.0);
            direct += uExtraColor[li].rgb * uExtraColor[li].a * NdLx;
        }
        ambient = albedo * mix(groundAmbient, skyAmbient, hemisphere) * bakedAO;
        Lo      = albedo * direct;
    }

    // Compose: ambient + direct + emissive (all in linear HDR space)
    vec3 color = ambient + Lo + emissive;

    // Apply fog in LINEAR HDR (bloom composite pass does ACES + gamma).
    // Clamp to finite range to prevent NaNs in the bloom downsample chain.
    color = ApplyFog(color, worldPos);
    color = clamp(color, vec3(0.0), vec3(1000.0));

    outColor = vec4(color, 1.0);
}
