cbuffer DebugLineCB : register(b0) {
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
    float4 Color    : COLOR;
};

VS_OUTPUT VS_Main(VS_INPUT input) {
    VS_OUTPUT output;
    output.Position = mul(float4(input.Position, 1.0), ViewProjection);
    output.Color = input.Color;
    return output;
}

float4 PS_Main(VS_OUTPUT input) : SV_Target {
    return input.Color;
}
