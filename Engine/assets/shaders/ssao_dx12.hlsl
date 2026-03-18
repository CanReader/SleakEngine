// ============================================================
// Screen Space Ambient Occlusion (SSAO) Shader (DirectX 12 HLSL)
// ============================================================

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

cbuffer SSAOCB : register(b0) {
    float ssaoRadius;
    float ssaoBias;
    float ssaoPower;
    uint  ssaoKernelSize;

    float screenWidth;
    float screenHeight;
    uint  ssaoEnabled;
    uint  _ssaoPad0;

    float nearPlane;
    float farPlane;
    float _ssaoPad1, _ssaoPad2;

    float _ssaoPad3[4];
};

cbuffer KernelCB : register(b1) {
    float4 ssaoKernel[64];
};

cbuffer CameraCB : register(b2) {
    row_major float4x4 Projection;
    row_major float4x4 View;
    row_major float4x4 InvProjection;
};

Texture2D depthTexture : register(t0);
Texture2D normalTexture : register(t1);
Texture2D noiseTexture : register(t2);

SamplerState pointClampSampler : register(s0);
SamplerState repeatSampler : register(s1);

VS_OUTPUT VS_Main(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.TexCoord = float2(uv.x, 1.0 - uv.y);
    return output;
}

float3 ReconstructViewPos(float2 uv, float depth) {
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewPos = mul(clipPos, InvProjection);
    return viewPos.xyz / viewPos.w;
}

float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    if (!ssaoEnabled)
        return float4(1.0, 1.0, 1.0, 1.0);

    float depth = depthTexture.Sample(pointClampSampler, input.TexCoord).r;
    if (depth >= 1.0)
        return float4(1.0, 1.0, 1.0, 1.0);

    float3 fragPos = ReconstructViewPos(input.TexCoord, depth);
    float3 normal = normalize(normalTexture.Sample(pointClampSampler, input.TexCoord).rgb * 2.0 - 1.0);

    float2 noiseScale = float2(screenWidth / 4.0, screenHeight / 4.0);
    float3 randomVec = noiseTexture.Sample(repeatSampler, input.TexCoord * noiseScale).xyz;

    float3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 TBN = float3x3(tangent, bitangent, normal);

    float occlusion = 0.0;

    for (uint i = 0; i < ssaoKernelSize; ++i) {
        float3 sampleDir = mul(ssaoKernel[i].xyz, TBN);
        float3 samplePos = fragPos + sampleDir * ssaoRadius;

        float4 offset = mul(float4(samplePos, 1.0), Projection);
        offset.xy /= offset.w;
        offset.xy = offset.xy * 0.5 + 0.5;
        offset.y = 1.0 - offset.y;

        float sampleDepth = depthTexture.Sample(pointClampSampler, offset.xy).r;
        float3 sampleViewPos = ReconstructViewPos(offset.xy, sampleDepth);

        float rangeCheck = smoothstep(0.0, 1.0,
            ssaoRadius / abs(fragPos.z - sampleViewPos.z));
        occlusion += (sampleViewPos.z >= samplePos.z + ssaoBias ? 1.0 : 0.0)
                   * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(ssaoKernelSize));
    occlusion = pow(occlusion, ssaoPower);

    return float4(occlusion, occlusion, occlusion, 1.0);
}
