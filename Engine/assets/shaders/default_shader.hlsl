// ============================================================
// DXCraft Basic Material Shader (DirectX 11 HLSL)
// Cook-Torrance PBR with multi-light support
// ============================================================

struct VS_INPUT
{
    float3 POSITION : POSITION;
    float3 NORMAL   : NORMAL;
    float4 TANGENT  : TANGENT;
    float4 COLOR    : COLOR;
    float2 TEXCOORD : TEXCOORD;
};

struct VS_OUTPUT
{
    float4 Position    : SV_POSITION;
    float3 WorldPos    : TEXCOORD0;
    float3 WorldNorm   : TEXCOORD1;
    float3 WorldTan    : TEXCOORD2;
    float3 WorldBit    : TEXCOORD3;
    float4 Color       : COLOR;
    float2 TexCoord    : TEXCOORD4;
    float4 ShadowCoord : TEXCOORD5;
};

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
    float4 FogColor;
    float FogStart;
    float FogEnd;
    float2 _reserved;
    LightData Lights[16];
};

Texture2D diffuseTexture   : register(t0);
Texture2D normalTexture    : register(t1);
Texture2D specularTexture  : register(t2);
Texture2D roughnessTexture : register(t3);
Texture2D metallicTexture  : register(t4);
Texture2D aoTexture        : register(t5);
Texture2D emissiveTexture  : register(t6);

SamplerState mainSampler : register(s0);

// Shadow mapping with PCSS (declared before VS so vertex shader can access ShadowLightVP)
cbuffer ShadowCB : register(b5) {
    row_major float4x4 ShadowLightVP;
    float  ShadowBias;
    float  ShadowStrength;
    float  ShadowTexelSize;
    float  ShadowLightSize;
    uint   PCSSEnabled;
    uint   ShadowMapEnabled;
    float2 _shadowPad;
};

VS_OUTPUT VS_Main(VS_INPUT input)
{
    VS_OUTPUT output;

    output.Position = mul(float4(input.POSITION, 1.0), WVP);

    // World-space position and basis vectors for lighting
    float4 worldPos = mul(float4(input.POSITION, 1.0), World);
    output.WorldPos = worldPos.xyz;

    output.WorldNorm = normalize(mul(input.NORMAL,
                                     (float3x3) World));
    output.WorldTan = normalize(mul(input.TANGENT.xyz,
                                    (float3x3) World));
    output.WorldBit = cross(output.WorldNorm, output.WorldTan)
                      * input.TANGENT.w;

    output.Color    = input.COLOR;
    output.TexCoord = input.TEXCOORD;

    // Shadow coordinate: transform world position by light VP
    output.ShadowCoord = mul(worldPos, ShadowLightVP);

    return output;
}

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

// Fresnel-Schlick with roughness (for IBL ambient specular)
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    float oneMinusRoughness = 1.0 - roughness;
    return F0 + (max(float3(oneMinusRoughness, oneMinusRoughness, oneMinusRoughness), F0) - F0)
        * pow(saturate(1.0 - cosTheta), 5.0);
}

// IBL textures
TextureCube irradianceMap   : register(t7);
TextureCube prefilterMap    : register(t8);
Texture2D   brdfLUT         : register(t9);

cbuffer IBLCB : register(b3) {
    uint  IBLEnabled;
    float IBLIntensity;
    float MaxReflectionLOD;
    uint  _iblPad0;
};

// SSAO composite texture (screen-space, blurred AO)
Texture2D ssaoTexture : register(t10);

cbuffer SSAOCB : register(b4) {
    uint  SSAOCompositeEnabled;
    float ssaoScreenWidth;
    float ssaoScreenHeight;
    float _ssaoCompPad;
};

// Shadow map textures and samplers (ShadowCB declared above VS_Main)
Texture2D shadowMapTexture : register(t11);
SamplerComparisonState shadowCmpSampler : register(s1);
SamplerState shadowPointSampler : register(s2);

// ---- PCSS Shadow Functions ----

// Interleaved gradient noise for per-pixel disk rotation
float InterleavedGradientNoise(float2 screenPos) {
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(screenPos, magic.xy)));
}

// 32-sample Poisson disk
static const float2 poissonDisk[32] = {
    float2(-0.9465, -0.1484), float2(-0.7431,  0.5353),
    float2(-0.5863, -0.5879), float2(-0.3935,  0.1025),
    float2(-0.2428,  0.7722), float2(-0.1074, -0.3075),
    float2( 0.0542, -0.8645), float2( 0.1267,  0.4300),
    float2( 0.2787, -0.1353), float2( 0.3842,  0.6501),
    float2( 0.4714, -0.5537), float2( 0.5765,  0.1675),
    float2( 0.6712, -0.3340), float2( 0.7527,  0.4813),
    float2( 0.8745, -0.0910), float2( 0.9601,  0.2637),
    float2(-0.8312,  0.3150), float2(-0.6142, -0.2890),
    float2(-0.4581,  0.6310), float2(-0.2134, -0.7520),
    float2(-0.0678,  0.4210), float2( 0.1893, -0.5430),
    float2( 0.3215,  0.8140), float2( 0.4890, -0.0230),
    float2( 0.6340,  0.3470), float2( 0.7810, -0.5690),
    float2(-0.3467,  0.2560), float2(-0.1290, -0.1120),
    float2( 0.0910,  0.1560), float2( 0.2340, -0.3410),
    float2(-0.5120,  0.0420), float2( 0.4210,  0.5120)
};

// PCSS Step 1: Average blocker depth
float FindBlockerDepthHLSL(float2 uv, float receiverDepth, float searchRadius,
                            float2 screenPos) {
    float blockerSum = 0.0;
    int blockerCount = 0;

    float angle = InterleavedGradientNoise(screenPos) * 6.283185;
    float sa = sin(angle);
    float ca = cos(angle);

    for (int i = 0; i < 16; i++) {
        float2 rotated = float2(
            ca * poissonDisk[i].x - sa * poissonDisk[i].y,
            sa * poissonDisk[i].x + ca * poissonDisk[i].y);
        float2 sampleUV = uv + rotated * searchRadius;

        float sampleDepth = shadowMapTexture.Sample(shadowPointSampler, sampleUV).r;
        if (sampleDepth < receiverDepth) {
            blockerSum += sampleDepth;
            blockerCount++;
        }
    }

    if (blockerCount == 0) return -1.0;
    return blockerSum / float(blockerCount);
}

// PCSS Step 2: Penumbra width estimation
float EstimatePenumbraWidthHLSL(float receiverDepth, float blockerDepth) {
    return ((receiverDepth - blockerDepth) / blockerDepth) * ShadowLightSize;
}

// PCSS Step 3: Variable-kernel PCF
float PCSSFilterHLSL(float2 uv, float receiverDepth, float filterRadius,
                      float2 screenPos) {
    float angle = InterleavedGradientNoise(screenPos) * 6.283185;
    float sa = sin(angle);
    float ca = cos(angle);

    float shadow = 0.0;
    for (int i = 0; i < 32; i++) {
        float2 rotated = float2(
            ca * poissonDisk[i].x - sa * poissonDisk[i].y,
            sa * poissonDisk[i].x + ca * poissonDisk[i].y);
        shadow += shadowMapTexture.SampleCmpLevelZero(
            shadowCmpSampler, uv + rotated * filterRadius, receiverDepth);
    }
    return shadow / 32.0;
}

// Main shadow calculation
float CalcShadowPCSS(float4 shadowCoord, float2 screenPos) {
    if (!ShadowMapEnabled) return 1.0;

    float3 proj = shadowCoord.xyz / shadowCoord.w;
    proj.xy = proj.xy * 0.5 + 0.5;
    proj.y = 1.0 - proj.y;

    if (proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0 ||
        proj.z < 0.0 || proj.z > 1.0)
        return 1.0;

    float2 fadeCoord = smoothstep(float2(0.0, 0.0), float2(0.05, 0.05), proj.xy)
                     * smoothstep(float2(0.0, 0.0), float2(0.05, 0.05), float2(1.0, 1.0) - proj.xy);
    float edgeFade = fadeCoord.x * fadeCoord.y;

    float biasedDepth = proj.z - ShadowBias;
    float shadow;

    if (PCSSEnabled) {
        float searchRadius = ShadowTexelSize * ShadowLightSize * 40.0;
        float blockerDepth = FindBlockerDepthHLSL(proj.xy, biasedDepth, searchRadius, screenPos);

        if (blockerDepth < 0.0) return 1.0;

        float penumbra = EstimatePenumbraWidthHLSL(biasedDepth, blockerDepth);
        float filterRadius = max(penumbra * ShadowTexelSize * 80.0, ShadowTexelSize * 8.0);
        filterRadius = min(filterRadius, ShadowTexelSize * ShadowLightSize * 30.0);

        shadow = PCSSFilterHLSL(proj.xy, biasedDepth, filterRadius, screenPos);
    } else {
        // Standard 3x3 PCF
        shadow = 0.0;
        float radius = ShadowTexelSize * 2.0;
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                shadow += shadowMapTexture.SampleCmpLevelZero(
                    shadowCmpSampler,
                    proj.xy + float2(float(x), float(y)) * radius,
                    biasedDepth);
            }
        }
        shadow /= 9.0;
    }

    return lerp(1.0, shadow, ShadowStrength * edgeFade);
}

// ---- End PCSS Shadow Functions ----

// UE4-style distance attenuation
float AttenuateUE4(float distance, float range) {
    if (range <= 0.0) return 1.0;
    float d = distance / range;
    float d2 = d * d;
    float d4 = d2 * d2;
    float falloff = saturate(1.0 - d4);
    return (falloff * falloff) / (distance * distance + 1.0);
}

float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    // Apply UV tiling and offset
    float2 uv = input.TexCoord * matTiling + matOffset;

    float4 baseColor = matDiffuseColor * input.Color;
    if (HasDiffuseMap)
        baseColor *= diffuseTexture.Sample(mainSampler, uv);

    float alpha = baseColor.a * matOpacity;
    if (matAlphaCutoff > 0.0 && alpha < matAlphaCutoff)
        discard;

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

    // Composite screen-space AO
    if (SSAOCompositeEnabled) {
        float2 screenUV = input.Position.xy / float2(ssaoScreenWidth, ssaoScreenHeight);
        float ssao = ssaoTexture.Sample(mainSampler, screenUV).r;
        ao *= ssao;
    }

    float3 albedo = baseColor.rgb;
    float3 V = normalize(CameraPos - input.WorldPos);

    // Reflectance at normal incidence (F0)
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, albedo, metallic);

    float3 Lo = float3(0.0, 0.0, 0.0);

    for (uint i = 0; i < NumActiveLights; i++) {
        LightData light = Lights[i];

        float3 L;
        float attenuation = 1.0;

        if (light.Type == 0) {
            // Directional light
            L = normalize(-light.Direction);
            // Apply PCSS shadow to first directional light
            if (i == 0u) {
                float shadow = CalcShadowPCSS(input.ShadowCoord, input.Position.xy);
                attenuation *= shadow;
            }
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

    float3 ambient;
    if (IBLEnabled) {
        // IBL diffuse: sample irradiance map
        float3 kS_ibl = FresnelSchlickRoughness(
            max(dot(N, V), 0.0), F0, roughness);
        float3 kD_ibl = (1.0 - kS_ibl) * (1.0 - metallic);

        float3 irradiance = irradianceMap.Sample(mainSampler, N).rgb;
        float3 diffuseIBL = irradiance * albedo;

        // IBL specular: sample prefiltered env map + BRDF LUT
        float3 R = reflect(-V, N);
        float3 prefilteredColor = prefilterMap.SampleLevel(
            mainSampler, R, roughness * MaxReflectionLOD).rgb;
        float NdotV_ibl = max(dot(N, V), 0.0);
        float2 envBRDF = brdfLUT.Sample(mainSampler,
            float2(NdotV_ibl, roughness)).rg;
        float3 specularIBL = prefilteredColor
            * (kS_ibl * envBRDF.x + envBRDF.y);

        ambient = (kD_ibl * diffuseIBL + specularIBL)
                * ao * IBLIntensity;
    } else {
        ambient = AmbientColor * AmbientIntensity
                * albedo * ao;
    }

    float3 emissive = matEmissiveColor * matEmissiveIntensity;
    if (HasEmissiveMap)
        emissive *= emissiveTexture.Sample(mainSampler, uv).rgb;

    float3 finalColor = ambient + Lo + emissive;

    return float4(finalColor, alpha);
}
