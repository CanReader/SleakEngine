// ============================================================
// Skinned Material Shader (DirectX 11 HLSL)
// Cook-Torrance PBR with multi-light + skeletal animation
// ============================================================

struct VS_INPUT
{
    float3 POSITION      : POSITION;
    float3 NORMAL        : NORMAL;
    float4 TANGENT       : TANGENT;
    float4 COLOR         : COLOR;
    float2 TEXCOORD      : TEXCOORD;
    int4   BLENDINDICES  : BLENDINDICES;
    float4 BLENDWEIGHT   : BLENDWEIGHT;
};

struct VS_OUTPUT
{
    float4 Position  : SV_POSITION;
    float3 WorldPos  : TEXCOORD0;
    float3 WorldNorm : TEXCOORD1;
    float3 WorldTan  : TEXCOORD2;
    float3 WorldBit  : TEXCOORD3;
    float4 Color     : COLOR;
    float2 TexCoord  : TEXCOORD4;
};

// --- Constant Buffers ---

cbuffer TransformCB : register(b0) {
    row_major float4x4 WVP;
    row_major float4x4 World;
};

cbuffer MaterialCB : register(b1) {
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
    float4 matDiffuseColor;

    // Row 3: specular RGB + shininess
    float3 matSpecularColor;
    float matShininess;

    // Row 4: emissive RGB + intensity
    float3 matEmissiveColor;
    float matEmissiveIntensity;

    // Row 5: PBR factors
    float matMetallic;
    float matRoughness;
    float matAO;
    float matNormalIntensity;

    // Row 6: UV transform
    float2 matTiling;
    float2 matOffset;

    // Row 7: alpha
    float matOpacity;
    float matAlphaCutoff;
    float2 _pad1;
};

struct LightData {
    float3 Position;  uint Type;
    float3 Direction; float Intensity;
    float3 Color;     float Range;
    float SpotInnerCos; float SpotOuterCos;
    float AreaWidth; float AreaHeight;
};

cbuffer LightCB : register(b2) {
    float3 CameraPos;
    uint NumActiveLights;
    float3 AmbientColor;
    float AmbientIntensity;
    float4 _reserved[2];
    LightData Lights[16];
};

// Bone matrices
static const int MAX_BONES = 256;
cbuffer BoneMatrices : register(b3) {
    row_major float4x4 boneMatrices[MAX_BONES];
};

// --- Textures & Samplers ---

Texture2D diffuseTexture   : register(t0);
Texture2D normalTexture    : register(t1);
Texture2D specularTexture  : register(t2);
Texture2D roughnessTexture : register(t3);
Texture2D metallicTexture  : register(t4);
Texture2D aoTexture        : register(t5);
Texture2D emissiveTexture  : register(t6);

SamplerState mainSampler : register(s0);

// ============================================================
// Vertex Shader
// ============================================================
VS_OUTPUT VS_Main(VS_INPUT input)
{
    VS_OUTPUT output;

    // Compute skin matrix
    float totalWeight = input.BLENDWEIGHT.x + input.BLENDWEIGHT.y +
                        input.BLENDWEIGHT.z + input.BLENDWEIGHT.w;

    float4x4 skinMatrix;
    if (totalWeight > 0.01) {
        skinMatrix = (float4x4)0;
        for (int i = 0; i < 4; i++) {
            if (input.BLENDINDICES[i] >= 0 && input.BLENDINDICES[i] < MAX_BONES)
                skinMatrix += boneMatrices[input.BLENDINDICES[i]] * input.BLENDWEIGHT[i];
        }
    } else {
        skinMatrix = float4x4(
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1);
    }

    float4 skinnedPos = mul(float4(input.POSITION, 1.0), skinMatrix);
    output.Position = mul(skinnedPos, WVP);

    // World-space position and basis vectors for lighting
    float4 worldPos = mul(skinnedPos, World);
    output.WorldPos = worldPos.xyz;

    float3x3 skinMat3 = (float3x3) skinMatrix;
    float3x3 worldMat3 = (float3x3) World;

    output.WorldNorm = normalize(mul(mul(input.NORMAL, skinMat3), worldMat3));
    output.WorldTan  = normalize(mul(mul(input.TANGENT.xyz, skinMat3), worldMat3));
    output.WorldBit  = cross(output.WorldNorm, output.WorldTan) * input.TANGENT.w;

    output.Color    = input.COLOR;
    output.TexCoord = input.TEXCOORD;

    return output;
}

// ============================================================
// PBR Helper Functions
// ============================================================

static const float PI = 3.14159265359;

// GGX/Trowbridge-Reitz normal distribution
float DistributionGGX(float3 N, float3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0000001);
}

// Smith-GGX geometry function (single direction)
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith geometry function (both view and light)
float GeometrySmith(float3 N, float3 V, float3 L,
                    float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Fresnel-Schlick approximation
float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(
        saturate(1.0 - cosTheta), 5.0);
}

// UE4-style distance attenuation
float AttenuateUE4(float distance, float range) {
    if (range <= 0.0) return 1.0;
    float d = distance / range;
    float d2 = d * d;
    float d4 = d2 * d2;
    float falloff = saturate(1.0 - d4);
    return (falloff * falloff) / (distance * distance + 1.0);
}

// ============================================================
// Pixel Shader
// ============================================================
float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    // Apply UV tiling and offset
    float2 uv = input.TexCoord * matTiling + matOffset;

    // --- Base Color ---
    float4 baseColor = matDiffuseColor * input.Color;
    if (HasDiffuseMap)
        baseColor *= diffuseTexture.Sample(mainSampler, uv);

    // --- Alpha Cutoff ---
    float alpha = baseColor.a * matOpacity;
    if (matAlphaCutoff > 0.0 && alpha < matAlphaCutoff)
        discard;

    // --- Normal ---
    float3 N = normalize(input.WorldNorm);
    if (HasNormalMap) {
        float3 tangentNormal =
            normalTexture.Sample(mainSampler, uv).xyz * 2.0 - 1.0;
        tangentNormal.xy *= matNormalIntensity;
        tangentNormal = normalize(tangentNormal);

        float3 T = normalize(input.WorldTan);
        float3 B = normalize(input.WorldBit);
        float3x3 TBN = float3x3(T, B, N);
        N = normalize(mul(tangentNormal, TBN));
    }

    // --- Material Properties ---
    float roughness = matRoughness;
    if (HasRoughnessMap)
        roughness *= roughnessTexture.Sample(mainSampler, uv).r;
    roughness = clamp(roughness, 0.04, 1.0);

    float metallic = matMetallic;
    if (HasMetallicMap)
        metallic *= metallicTexture.Sample(mainSampler, uv).r;

    float ao = matAO;
    if (HasAOMap)
        ao *= aoTexture.Sample(mainSampler, uv).r;

    // --- PBR Setup ---
    float3 albedo = baseColor.rgb;
    float3 V = normalize(CameraPos - input.WorldPos);

    // Reflectance at normal incidence (F0)
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, albedo, metallic);

    // --- Accumulate Light Contributions ---
    float3 Lo = float3(0.0, 0.0, 0.0);

    for (uint i = 0; i < NumActiveLights; i++) {
        LightData light = Lights[i];

        float3 L;
        float attenuation = 1.0;

        if (light.Type == 0) {
            // Directional light
            L = normalize(-light.Direction);
        }
        else if (light.Type == 1) {
            // Point light
            float3 toLight = light.Position - input.WorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            attenuation = AttenuateUE4(dist, light.Range);
        }
        else if (light.Type == 2) {
            // Spot light
            float3 toLight = light.Position - input.WorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            attenuation = AttenuateUE4(dist, light.Range);

            // Spot cone falloff
            float theta = dot(L, normalize(-light.Direction));
            float epsilon = light.SpotInnerCos
                          - light.SpotOuterCos;
            float spotFactor = saturate(
                (theta - light.SpotOuterCos)
                / max(epsilon, 0.0001));
            attenuation *= spotFactor;
        }
        else {
            // Area light (representative point approx)
            float3 toLight = light.Position - input.WorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            attenuation = AttenuateUE4(dist, light.Range);
        }

        float3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);

        if (NdotL > 0.0) {
            // Cook-Torrance BRDF
            float  D = DistributionGGX(N, H, roughness);
            float  G = GeometrySmith(N, V, L, roughness);
            float3 F = FresnelSchlick(
                max(dot(H, V), 0.0), F0);

            float3 numerator = D * G * F;
            float denominator = 4.0 * max(dot(N, V), 0.0)
                              * NdotL + 0.0001;
            float3 specular = numerator / denominator;

            // Energy conservation
            float3 kS = F;
            float3 kD = (1.0 - kS) * (1.0 - metallic);

            float3 radiance = light.Color * light.Intensity
                            * attenuation;

            Lo += (kD * albedo / PI + specular)
                * radiance * NdotL;
        }
    }

    // --- Ambient ---
    float3 ambient = AmbientColor * AmbientIntensity
                   * albedo * ao;

    // --- Emissive ---
    float3 emissive = matEmissiveColor * matEmissiveIntensity;
    if (HasEmissiveMap)
        emissive *= emissiveTexture.Sample(mainSampler, uv).rgb;

    // --- Final Composition ---
    float3 finalColor = ambient + Lo + emissive;

    return float4(finalColor, alpha);
}
