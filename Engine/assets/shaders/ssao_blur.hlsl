// ============================================================
// SSAO Blur Pass (DirectX 11 HLSL)
// Simple 4x4 box blur to smooth SSAO noise
// ============================================================

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

Texture2D ssaoInput : register(t0);
SamplerState pointClampSampler : register(s0);

VS_OUTPUT VS_Main(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.TexCoord = float2(uv.x, 1.0 - uv.y);
    return output;
}

float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    float2 texelSize = 1.0 / float2(
        ssaoInput.GetDimensions(0, texelSize.x, texelSize.y));

    // Recompute texelSize properly
    uint w, h;
    ssaoInput.GetDimensions(w, h);
    texelSize = 1.0 / float2(w, h);

    float result = 0.0;
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            float2 offset = float2(float(x), float(y)) * texelSize;
            result += ssaoInput.Sample(pointClampSampler,
                input.TexCoord + offset).r;
        }
    }
    result /= 16.0;

    return float4(result, result, result, 1.0);
}
