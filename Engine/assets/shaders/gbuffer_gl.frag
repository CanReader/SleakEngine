#version 450 core

// GBuffer geometry pass — outputs material properties to 3 render targets.
// RT0: AlbedoRGB + AO           (RGBA8)
// RT1: WorldNormalXYZ + Rough   (RGBA16F, normal packed to [0,1])
// RT2: Metallic + EmissiveScale  (RGBA8)

in vec3 fragWorldPos;
in vec3 fragWorldNorm;
in vec3 fragWorldTan;
in vec3 fragWorldBit;
in vec4 fragColor;
in vec2 fragUV;
in vec4 fragShadowCoord;

layout(location = 0) out vec4 outAlbedoAO;
layout(location = 1) out vec4 outNormalRough;
layout(location = 2) out vec4 outMetalEmit;
layout(location = 3) out vec4 outWorldPos;

// Material textures (binding must match TEXTURE_SLOT defines)
layout(binding = 0) uniform sampler2D diffuseTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D specularTexture;
layout(binding = 3) uniform sampler2D roughnessTexture;
layout(binding = 4) uniform sampler2D metallicTexture;
layout(binding = 5) uniform sampler2D aoTexture;
layout(binding = 6) uniform sampler2D emissiveTexture;

// Material UBO (binding 1) - matches MaterialGPUData layout
layout(std140, binding = 1) uniform MaterialUBO {
    uint HasDiffuseMap;
    uint HasNormalMap;
    uint HasSpecularMap;
    uint HasRoughnessMap;
    uint HasMetallicMap;
    uint HasAOMap;
    uint HasEmissiveMap;
    uint _pad0;
    vec4 matDiffuseColor;
    vec3 matSpecularColor;
    float matShininess;
    vec3 matEmissiveColor;
    float matEmissiveIntensity;
    float matMetallic;
    float matRoughness;
    float matAO;
    float matNormalIntensity;
    vec2 matTiling;
    vec2 matOffset;
    float matOpacity;
    float matAlphaCutoff;
    vec2 _pad1;
};

void main() {
    vec2 uv = fragUV * matTiling + matOffset;

    // Albedo
    vec4 albedo = (HasDiffuseMap != 0u) ? texture(diffuseTexture, uv) : matDiffuseColor;
    if (albedo.a < matAlphaCutoff) discard;

    // Normal (world space)
    vec3 N = normalize(fragWorldNorm);
    if (HasNormalMap != 0u) {
        vec3 tn  = texture(normalTexture, uv).rgb * 2.0 - 1.0;
        tn.xy   *= matNormalIntensity;
        vec3 T   = normalize(fragWorldTan);
        vec3 B   = normalize(fragWorldBit);
        N        = normalize(T * tn.x + B * tn.y + N * tn.z);
    }

    // PBR scalars
    float roughness  = (HasRoughnessMap != 0u) ? texture(roughnessTexture, uv).r : matRoughness;
    float metallic   = (HasMetallicMap  != 0u) ? texture(metallicTexture,  uv).r : matMetallic;
    float ao         = (HasAOMap        != 0u) ? texture(aoTexture,        uv).r : (matAO * fragColor.r);
    float emitScale  = (HasEmissiveMap  != 0u) ? length(texture(emissiveTexture, uv).rgb) :
                        length(matEmissiveColor) * matEmissiveIntensity;

    outAlbedoAO    = vec4(albedo.rgb, ao);
    outNormalRough = vec4(N * 0.5 + 0.5, roughness);  // pack normal to [0,1]
    outMetalEmit   = vec4(metallic, clamp(emitScale, 0.0, 1.0), 0.0, 1.0);
    outWorldPos    = vec4(fragWorldPos, 1.0);
}
