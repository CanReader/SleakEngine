// ============================================================
// IBL Prefilter Environment Map (DirectX 11 HLSL)
// Generates mip-chain of pre-filtered specular cubemap
// Each mip level corresponds to a roughness level
// ============================================================

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float3 LocalPos : TEXCOORD0;
};

cbuffer PrefilterCB : register(b0) {
    row_major float4x4 ViewProjection;
    float Roughness;
    float3 _pfPad;
};

VS_OUTPUT VS_Main(float3 position : POSITION)
{
    VS_OUTPUT output;
    output.LocalPos = position;
    output.Position = mul(float4(position, 1.0), ViewProjection);
    return output;
}

static const float PI = 3.14159265359;

TextureCube environmentMap : register(t0);
SamplerState envSampler : register(s0);

float RadicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint N) {
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness) {
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float DistributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    return a2 / max(denom, 0.0000001);
}

float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    float3 N = normalize(input.LocalPos);
    // Assume view direction equals reflection direction
    float3 R = N;
    float3 V = R;

    static const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    float3 prefilteredColor = float3(0.0, 0.0, 0.0);

    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, Roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            // Sample with mip bias based on PDF to reduce aliasing
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float D = DistributionGGX(NdotH, Roughness);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;

            // Compute mip level from solid angle ratio
            float resolution = 512.0; // environment map resolution
            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);
            float mipLevel = Roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

            prefilteredColor += environmentMap.SampleLevel(envSampler, L, mipLevel).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor /= max(totalWeight, 0.001);

    return float4(prefilteredColor, 1.0);
}
