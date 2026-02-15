TextureCube skyboxTexture : register(t0);
SamplerState samplerState : register(s0);

cbuffer SkyboxCB : register(b0) {
    row_major float4x4 ViewProjection;
};

struct VS_INPUT {
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float4 Tangent  : TANGENT;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD;
};

struct VS_OUTPUT {
    float4 Position : SV_POSITION;
    float3 TexCoord : TEXCOORD0;
};

VS_OUTPUT VS_Main(VS_INPUT input) {
    VS_OUTPUT output;
    output.Position = mul(float4(input.Position, 1.0), ViewProjection);
    output.Position = output.Position.xyww;
    output.TexCoord = input.Position;
    return output;
}

float4 PS_Main(VS_OUTPUT input) : SV_Target {
    return skyboxTexture.Sample(samplerState, input.TexCoord);
}
