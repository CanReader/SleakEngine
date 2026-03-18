// ============================================================
// PCSS Soft Shadows (DirectX 11 HLSL)
// Percentage-Closer Soft Shadows with blocker search,
// penumbra estimation, and variable-kernel PCF filtering
// ============================================================

struct PCSS_VS_OUTPUT
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

cbuffer ShadowCB : register(b5) {
    row_major float4x4 ShadowLightVP;
    float4 ShadowLightDir;       // xyz = direction, w = normal bias
    float4 ShadowLightColor;     // rgb = color, a = intensity
    float  ShadowBias;
    float  ShadowStrength;
    float  ShadowTexelSize;
    float  ShadowLightSize;      // world-space light size for PCSS
    uint   PCSSEnabled;
    float3 _shadowPad;
};

Texture2D shadowMapTexture : register(t11);
SamplerComparisonState shadowSampler : register(s1);
SamplerState shadowPointSampler : register(s2);

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

// PCSS Step 1: Blocker search using first 16 samples
float FindAverageBlockerDepth(float2 uv, float receiverDepth, float searchRadius) {
    float blockerSum = 0.0;
    int blockerCount = 0;

    float angle = InterleavedGradientNoise(uv * 4096.0) * 6.283185;
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

    if (blockerCount == 0)
        return -1.0; // No blockers

    return blockerSum / float(blockerCount);
}

// PCSS Step 2: Penumbra width estimation
float EstimatePenumbraWidth(float receiverDepth, float blockerDepth) {
    float penumbra = (receiverDepth - blockerDepth) / blockerDepth;
    return penumbra * ShadowLightSize;
}

// PCSS Step 3: Variable-kernel PCF filtering
float PCSSFilter(float2 uv, float receiverDepth, float filterRadius) {
    float angle = InterleavedGradientNoise(uv * 4096.0) * 6.283185;
    float sa = sin(angle);
    float ca = cos(angle);

    float shadow = 0.0;
    for (int i = 0; i < 32; i++) {
        float2 rotated = float2(
            ca * poissonDisk[i].x - sa * poissonDisk[i].y,
            sa * poissonDisk[i].x + ca * poissonDisk[i].y);
        float2 sampleUV = uv + rotated * filterRadius;

        shadow += shadowMapTexture.SampleCmpLevelZero(
            shadowSampler, sampleUV, receiverDepth);
    }
    return shadow / 32.0;
}

// Simple hard shadow fallback (standard PCF)
float HardShadowPCF(float2 uv, float receiverDepth) {
    float shadow = 0.0;
    float radius = ShadowTexelSize * 2.0;

    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            float2 offset = float2(float(x), float(y)) * radius;
            shadow += shadowMapTexture.SampleCmpLevelZero(
                shadowSampler, uv + offset, receiverDepth);
        }
    }
    return shadow / 9.0;
}

// Main shadow calculation entry point
float CalculatePCSSShadow(float4 shadowCoord, float2 screenPos) {
    float3 projCoords = shadowCoord.xyz / shadowCoord.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    projCoords.y = 1.0 - projCoords.y; // DX UV convention

    // Out-of-bounds check
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0)
        return 1.0;

    // Edge fade
    float2 fadeCoord = smoothstep(float2(0.0, 0.0), float2(0.05, 0.05), projCoords.xy)
                     * smoothstep(float2(0.0, 0.0), float2(0.05, 0.05), float2(1.0, 1.0) - projCoords.xy);
    float edgeFade = fadeCoord.x * fadeCoord.y;

    float biasedDepth = projCoords.z - ShadowBias;

    float shadow;
    if (PCSSEnabled) {
        // PCSS: variable penumbra soft shadows
        float searchRadius = ShadowTexelSize * ShadowLightSize * 40.0;
        float blockerDepth = FindAverageBlockerDepth(projCoords.xy, biasedDepth, searchRadius);

        if (blockerDepth < 0.0) {
            // No blockers — fully lit
            return 1.0;
        }

        float penumbra = EstimatePenumbraWidth(biasedDepth, blockerDepth);
        float filterRadius = max(penumbra * ShadowTexelSize * 80.0, ShadowTexelSize * 8.0);
        filterRadius = min(filterRadius, ShadowTexelSize * ShadowLightSize * 30.0);

        shadow = PCSSFilter(projCoords.xy, biasedDepth, filterRadius);
    } else {
        // Fallback: hard shadow with basic PCF
        shadow = HardShadowPCF(projCoords.xy, biasedDepth);
    }

    shadow = lerp(1.0, shadow, ShadowStrength * edgeFade);
    return shadow;
}
