#version 450 core

// ============================================================
// DXCraft Basic Material Shader - OpenGL Fragment Shader
// Cook-Torrance PBR with multi-light support
// ============================================================

in vec3 fragWorldPos;
in vec3 fragWorldNorm;
in vec3 fragWorldTan;
in vec3 fragWorldBit;
in vec4 fragColor;
in vec2 fragUV;

out vec4 outColor;

// Material UBO (binding 1) - matches MaterialGPUData layout
layout(std140, binding = 1) uniform MaterialUBO {
    // Row 0: texture presence flags
    uint HasDiffuseMap;
    uint HasNormalMap;
    uint HasSpecularMap;
    uint HasRoughnessMap;

    // Row 1: more flags
    uint HasMetallicMap;
    uint HasAOMap;
    uint HasEmissiveMap;
    uint _pad0;

    // Row 2: diffuse color RGBA
    vec4 matDiffuseColor;

    // Row 3: specular RGB + shininess
    vec3 matSpecularColor;
    float matShininess;

    // Row 4: emissive RGB + intensity
    vec3 matEmissiveColor;
    float matEmissiveIntensity;

    // Row 5: PBR factors
    float matMetallic;
    float matRoughness;
    float matAO;
    float matNormalIntensity;

    // Row 6: UV transform
    vec2 matTiling;
    vec2 matOffset;

    // Row 7: alpha
    float matOpacity;
    float matAlphaCutoff;
    vec2 _pad1;
};

struct LightData {
    vec3 Position;  uint Type;
    vec3 Direction; float Intensity;
    vec3 Color;     float Range;
    float SpotInnerCos; float SpotOuterCos;
    float AreaWidth; float AreaHeight;
};

layout(std140, binding = 2) uniform LightUBO {
    vec3 CameraPos;
    uint NumActiveLights;
    vec3 AmbientColor;
    float AmbientIntensity;
    vec4 _reserved[2];
    LightData Lights[16];
};

// Texture samplers — binding must match TEXTURE_SLOT defines
layout(binding = 0) uniform sampler2D diffuseTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D specularTexture;
layout(binding = 3) uniform sampler2D roughnessTexture;
layout(binding = 4) uniform sampler2D metallicTexture;
layout(binding = 5) uniform sampler2D aoTexture;
layout(binding = 6) uniform sampler2D emissiveTexture;

// ============================================================
// PBR Helper Functions
// ============================================================

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(
        clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float AttenuateUE4(float distance, float range) {
    if (range <= 0.0) return 1.0;
    float d = distance / range;
    float d2 = d * d;
    float d4 = d2 * d2;
    float falloff = clamp(1.0 - d4, 0.0, 1.0);
    return (falloff * falloff) / (distance * distance + 1.0);
}

void main() {
    // Apply UV tiling and offset
    vec2 uv = fragUV * matTiling + matOffset;

    // --- Base Color ---
    vec4 baseColor = matDiffuseColor * fragColor;
    if (HasDiffuseMap != 0u)
        baseColor *= texture(diffuseTexture, uv);

    // --- Alpha Cutoff ---
    float alpha = baseColor.a * matOpacity;
    if (matAlphaCutoff > 0.0 && alpha < matAlphaCutoff)
        discard;

    // --- Normal ---
    vec3 N = normalize(fragWorldNorm);
    if (HasNormalMap != 0u) {
        vec3 tangentNormal =
            texture(normalTexture, uv).xyz * 2.0 - 1.0;
        tangentNormal.xy *= matNormalIntensity;
        tangentNormal = normalize(tangentNormal);

        vec3 T = normalize(fragWorldTan);
        vec3 B = normalize(fragWorldBit);
        mat3 TBN = mat3(T, B, N);
        N = normalize(TBN * tangentNormal);
    }

    // --- Material Properties ---
    float roughness = matRoughness;
    if (HasRoughnessMap != 0u)
        roughness *= texture(roughnessTexture, uv).r;
    roughness = clamp(roughness, 0.04, 1.0);

    float metallic = matMetallic;
    if (HasMetallicMap != 0u)
        metallic *= texture(metallicTexture, uv).r;

    float ao = matAO;
    if (HasAOMap != 0u)
        ao *= texture(aoTexture, uv).r;

    // --- PBR Setup ---
    vec3 albedo = baseColor.rgb;
    vec3 V = normalize(CameraPos - fragWorldPos);

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // --- Accumulate Light Contributions ---
    vec3 Lo = vec3(0.0);

    for (uint i = 0u; i < NumActiveLights; i++) {
        LightData light = Lights[i];

        vec3 L;
        float attenuation = 1.0;

        if (light.Type == 0u) {
            L = normalize(-light.Direction);
        }
        else if (light.Type == 1u) {
            vec3 toLight = light.Position - fragWorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            attenuation = AttenuateUE4(dist, light.Range);
        }
        else if (light.Type == 2u) {
            vec3 toLight = light.Position - fragWorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            attenuation = AttenuateUE4(dist, light.Range);

            float theta = dot(L, normalize(-light.Direction));
            float epsilon = light.SpotInnerCos
                          - light.SpotOuterCos;
            float spotFactor = clamp(
                (theta - light.SpotOuterCos)
                / max(epsilon, 0.0001), 0.0, 1.0);
            attenuation *= spotFactor;
        }
        else {
            vec3 toLight = light.Position - fragWorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            attenuation = AttenuateUE4(dist, light.Range);
        }

        vec3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);

        if (NdotL > 0.0) {
            float  D = DistributionGGX(N, H, roughness);
            float  G = GeometrySmith(N, V, L, roughness);
            vec3   F = FresnelSchlick(
                max(dot(H, V), 0.0), F0);

            vec3 numerator = D * G * F;
            float denominator = 4.0 * max(dot(N, V), 0.0)
                              * NdotL + 0.0001;
            vec3 specular = numerator / denominator;

            vec3 kS = F;
            vec3 kD = (1.0 - kS) * (1.0 - metallic);

            vec3 radiance = light.Color * light.Intensity
                          * attenuation;

            Lo += (kD * albedo / PI + specular)
                * radiance * NdotL;
        }
    }

    // --- Ambient ---
    vec3 ambient = AmbientColor * AmbientIntensity
                 * albedo * ao;

    // --- Emissive ---
    vec3 emissive = matEmissiveColor * matEmissiveIntensity;
    if (HasEmissiveMap != 0u)
        emissive *= texture(emissiveTexture, uv).rgb;

    // --- Final Composition ---
    vec3 finalColor = ambient + Lo + emissive;

    outColor = vec4(finalColor, alpha);
}
