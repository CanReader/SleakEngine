// ============================================================
// Default Material Shader (DirectX 12 HLSL)
// Directional light + Blinn-Phong specular + distance fog
// Lighting data from LightCB at register(b2)
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

// Light/fog CB — root parameter 3, register(b2)
// Layout matches ShadowLightUBO from ConstantBuffer.hpp
cbuffer LightCB : register(b2) {
    float4 LightDir;        // xyz = direction, w = shadow normal bias
    float4 LightColor;      // rgb = color, a = intensity
    float4 Ambient;         // rgb = color, a = intensity
    float4 CameraPos;       // xyz = position
    row_major float4x4 LightVP;
    float ShadowBias;
    float ShadowStrength;
    float ShadowTexelSize;
    float LightSize;
    float4 FogColor;        // rgb = color
    float FogStart;
    float FogEnd;
    float2 _fogPad;
};

// Texture + sampler — root parameter 2, register(t0/s0)
Texture2D diffuseTexture : register(t0);
SamplerState mainSampler : register(s0);

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

float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    float4 texColor = diffuseTexture.Sample(mainSampler,
                                             input.TexCoord);
    if (texColor.a < 0.5)
        discard;
    float4 baseColor = texColor * input.Color;

    float3 lightDir = normalize(LightDir.xyz);
    float3 lightColor = LightColor.rgb;
    float lightIntensity = LightColor.a;

    // Ambient
    float3 ambient = Ambient.rgb * Ambient.a;

    // Diffuse (Lambert)
    float3 N = normalize(input.WorldNorm);
    float NdotL = max(dot(N, -lightDir), 0.0);
    float3 diffuse = lightColor * lightIntensity * NdotL;

    // Simple specular (Blinn-Phong)
    float3 viewDir = normalize(CameraPos.xyz - input.WorldPos);
    float3 halfDir = normalize(-lightDir + viewDir);
    float spec = pow(max(dot(N, halfDir), 0.0), 32.0);
    float3 specular = lightColor * spec * 0.3;

    float3 finalColor = baseColor.rgb * (ambient + diffuse)
                      + specular;

    // Distance fog (Minecraft-style linear fog)
    if (FogEnd > 0.0) {
        float dist = length(input.WorldPos - CameraPos.xyz);
        float fogFactor = saturate((FogEnd - dist) / (FogEnd - FogStart));
        finalColor = lerp(FogColor.rgb, finalColor, fogFactor);
    }

    return float4(finalColor, baseColor.a);
}
