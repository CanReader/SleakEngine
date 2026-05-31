#version 450

// ============================================================
// GBuffer Geometry Pass — PBR Material Sampling
//
// Set 0, bindings 0-5: PBR texture maps
// Set 0, binding   6:  MaterialParams UBO (scalar fallbacks + flags)
//
// RT0 (location=0): AlbedoAO    — baseColor.rgb + AO.a
// RT1 (location=1): NormalRough — encoded world-normal.xyz + roughness.a
// RT2 (location=2): MetalEmit   — metallic.r + emissive.gba (R16G16B16A16_SFLOAT)
// RT3 (location=3): WorldPos    — world-space position.xyz + 1.a
// ============================================================

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNorm;
layout(location = 2) in vec3 fragWorldTan;
layout(location = 3) in vec3 fragWorldBit;
layout(location = 4) in vec4 fragColor;
layout(location = 5) in vec2 fragUV;

// -- Set 0: PBR material textures (bindings 0-5) --------------------
layout(set = 0, binding = 0) uniform sampler2D texDiffuse;
layout(set = 0, binding = 1) uniform sampler2D texNormal;
layout(set = 0, binding = 2) uniform sampler2D texMetallic;
layout(set = 0, binding = 3) uniform sampler2D texRoughness;
layout(set = 0, binding = 4) uniform sampler2D texAO;
layout(set = 0, binding = 5) uniform sampler2D texEmissive;

// -- Set 0: per-material scalar parameters (binding 6) --------------
layout(set = 0, binding = 6) uniform MaterialParams {
    vec4  albedoFactor;      // xyz = base color tint, w = opacity
    float metallicFactor;    // scalar multiplier (or value when no map)
    float roughnessFactor;   // scalar multiplier (or value when no map)
    float aoFactor;          // AO multiplier
    float normalIntensity;   // normal map XY scale
    vec4  emissiveFactor;    // xyz = emissive color tint, w = intensity
    float tilingX;
    float tilingY;
    float offsetX;
    float offsetY;
    uint  hasNormalMap;
    uint  hasMetallicMap;
    uint  hasRoughnessMap;
    uint  hasAOMap;
    uint  hasEmissiveMap;
    float _pad0;
    float _pad1;
    float _pad2;
};

layout(location = 0) out vec4 outAlbedoAO;
layout(location = 1) out vec4 outNormalRough;
layout(location = 2) out vec4 outMetalEmit;
layout(location = 3) out vec4 outWorldPos;

void main() {
    vec2 uv = fragUV * vec2(tilingX, tilingY) + vec2(offsetX, offsetY);

    // --- Base Colour / Albedo ---
    vec4 albedoSample = texture(texDiffuse, uv) * albedoFactor;
    if (albedoSample.a < 0.5) discard;

    // --- Normal (TBN basis) ---
    vec3 N = normalize(fragWorldNorm);
    if (hasNormalMap != 0u) {
        vec3 tN = texture(texNormal, uv).xyz * 2.0 - 1.0;
        tN.xy  *= normalIntensity;
        // Reconstruct Z from XY (handles BC5/compressed 2-channel normals and
        // also guards against non-unit samples from storage compression).
        tN.z = sqrt(max(1.0 - dot(tN.xy, tN.xy), 0.0));
        // Gram-Schmidt re-orthogonalise T against the interpolated N to
        // fix drift at high polygon angles. Use the interpolated bitangent
        // (sign-correct per tangent.w) to preserve handedness for mirrored UVs.
        vec3 T = normalize(fragWorldTan - dot(fragWorldTan, N) * N);
        vec3 B = normalize(fragWorldBit - dot(fragWorldBit, N) * N - dot(fragWorldBit, T) * T);
        N = normalize(mat3(T, B, N) * tN);
    }

    // --- Metallic ---
    float metallic = hasMetallicMap != 0u
        ? texture(texMetallic, uv).r * metallicFactor
        : metallicFactor;
    metallic = clamp(metallic, 0.0, 1.0);

    // --- Roughness ---
    // Clamp away from 0 (perfect mirror) to avoid NaN in the BRDF denominator.
    float roughness = hasRoughnessMap != 0u
        ? texture(texRoughness, uv).r * roughnessFactor
        : roughnessFactor;
    roughness = clamp(roughness, 0.04, 1.0);

    // --- Ambient Occlusion ---
    float ao = hasAOMap != 0u
        ? texture(texAO, uv).r * aoFactor
        : fragColor.r * aoFactor;   // vertex AO baked into red channel
    ao = clamp(ao, 0.0, 1.0);

    // --- Emissive (stored HDR in R16G16B16A16_SFLOAT RT2 .gba) ---
    vec3 emissive = hasEmissiveMap != 0u
        ? texture(texEmissive, uv).rgb * emissiveFactor.rgb * emissiveFactor.w
        : emissiveFactor.rgb * emissiveFactor.w;

    // Write GBuffer
    outAlbedoAO    = vec4(albedoSample.rgb, ao);
    outNormalRough = vec4(N * 0.5 + 0.5, roughness);
    outMetalEmit   = vec4(metallic, emissive);  // .r=metallic, .gba=emissiveRGB (HDR)
    outWorldPos    = vec4(fragWorldPos, 1.0);
}
