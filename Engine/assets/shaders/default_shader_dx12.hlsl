// ============================================================
// DXCraft Basic Material Shader (DirectX 12 HLSL)
// Hardcoded directional light fallback
// (DX12 root signature does not yet support Material/Light CBs)
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
    float4 Position  : SV_POSITION;
    float3 WorldPos  : TEXCOORD0;
    float3 WorldNorm : TEXCOORD1;
    float3 WorldTan  : TEXCOORD2;
    float3 WorldBit  : TEXCOORD3;
    float4 Color     : COLOR;
    float2 TexCoord  : TEXCOORD4;
};

// Transform CB — root parameter 0, register(b0)
cbuffer TransformCB : register(b0) {
    row_major float4x4 WVP;
    row_major float4x4 World;
};

// Texture + sampler — root parameter 1, register(t0/s0)
Texture2D diffuseTexture : register(t0);
SamplerState mainSampler : register(s0);

// ============================================================
// Vertex Shader
// ============================================================
VS_OUTPUT VS_Main(VS_INPUT input)
{
    VS_OUTPUT output;

    output.Position = mul(float4(input.POSITION, 1.0), WVP);

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

    return output;
}

// ============================================================
// Pixel Shader
// ============================================================
float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    // Sample texture
    float4 texColor = diffuseTexture.Sample(mainSampler,
                                             input.TexCoord);
    float4 baseColor = texColor * input.Color;

    // Hardcoded directional light (sun-like)
    float3 lightDir = normalize(float3(-0.3, -1.0, -0.4));
    float3 lightColor = float3(1.0, 0.95, 0.9);
    float lightIntensity = 1.2;

    // Ambient
    float3 ambient = float3(0.15, 0.15, 0.18);

    // Diffuse (Lambert)
    float3 N = normalize(input.WorldNorm);
    float NdotL = max(dot(N, -lightDir), 0.0);
    float3 diffuse = lightColor * lightIntensity * NdotL;

    // Simple specular (Blinn-Phong)
    float3 viewDir = normalize(-input.WorldPos);
    float3 halfDir = normalize(-lightDir + viewDir);
    float spec = pow(max(dot(N, halfDir), 0.0), 32.0);
    float3 specular = lightColor * spec * 0.3;

    float3 finalColor = baseColor.rgb * (ambient + diffuse)
                      + specular;

    return float4(finalColor, baseColor.a);
}
