// ============================================================
// ACES Tonemapping Post-Process Shader (DirectX 12 HLSL)
// Fullscreen triangle pass: HDR -> LDR with ACES filmic curve
// ============================================================

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

cbuffer PostProcessCB : register(b0) {
    float Exposure;
    float Gamma;
    uint  TonemapEnabled;
    uint  _ppPad0;
};

Texture2D sceneTexture : register(t0);
SamplerState pointSampler : register(s0);

VS_OUTPUT VS_Main(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;

    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.TexCoord = float2(uv.x, 1.0 - uv.y);

    return output;
}

float3 ACESFilm(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    float3 hdr = sceneTexture.Sample(pointSampler, input.TexCoord).rgb;

    if (TonemapEnabled) {
        hdr *= Exposure;
        float3 ldr = ACESFilm(hdr);
        ldr = pow(ldr, 1.0 / Gamma);
        return float4(ldr, 1.0);
    }

    return float4(hdr, 1.0);
}
