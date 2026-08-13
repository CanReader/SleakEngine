Texture2D DiffuseTexture : register(t0);
Texture2D SpecularTexture : register(t1);
Texture2D NormalTexture : register(t2);
Texture2D RoughnessTexture : register(t3);
Texture2D MetalicTexture : register(t4);
Texture2D AOTexture : register(t5);
Texture2D EmmisiveTexture : register(t6);

SamplerState diffuseState : register(s0);
SamplerState specularState : register(s1);
SamplerState normalState : register(s2);
SamplerState roughnessState : register(s3);
SamplerState metalicState : register(s4);
SamplerState AOState : register(s5);
SamplerState EmmisiveState : register(s6);

#define MAX_DIRECTIONAL_LIGHTS 10
#define MAX_POINT_LIGHTS 10
#define MAX_SPOT_LIGHTS 10

struct VS_INPUT
{
    float3 POSITION : POSITION;
    float3 NORMAL : NORMAL;
    float4 TANGENT : TANGENT;
    float4 COLOR : COLOR;
    float2 TEXCOORD : TEXCOORD;
};

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    float3 Bitangent : BITANGENT;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD1;
};

struct LightCommon {
    float3 Color;
    float Intensity;
    float SpecularPower;
};

struct DirectionalLight : LightCommon {
    float3 Direction;
};

struct PointLight : LightCommon {
    float3 Position;
    float Range;
    float Attenuation;
};

struct SpotLight : PointLight {
    float ConeAngle;
    float3 Direction;
};

cbuffer Transform : register(b0) {
    row_major float4x4 WVP;
    row_major float4x4 World;
};

cbuffer MaterialBuffer : register(b1) {
   		bool HasDiffuse;
        bool HasSpecular;
        bool HasNormal;
        bool HasRoughness;
        bool HasMetalic;
        bool HasAO;
        bool HasEmissive;

        float4 DiffuseColor;
        float4 SpecularColor;
        float Shininess;
}

cbuffer LightBuffer : register(b2)
{
    DirectionalLight directionalLights[MAX_DIRECTIONAL_LIGHTS];
    PointLight pointLights[MAX_POINT_LIGHTS];
    SpotLight spotLights[MAX_SPOT_LIGHTS];
    
    int numDirectionalLights;
    int numPointLights;
    int numSpotLights;
    
    float3 AmbientColor;
    float AmbientIntensity;
};

// Vertex Shader
VS_OUTPUT VS_Main(VS_INPUT input)
{
    VS_OUTPUT output;
    
    output.Position = mul(float4(input.POSITION, 1.0), WVP);
    output.WorldPos = mul(float4(input.POSITION, 1.0), World).xyz;
    output.Normal = normalize(mul(float4(input.NORMAL, 0.0), World).xyz);
    output.Tangent = normalize(mul(float4(input.TANGENT.xyz, 0.0), World).xyz);
    output.Bitangent = cross(output.Normal, output.Tangent) * input.TANGENT.w;
    output.Color = input.COLOR;
    output.TexCoord = input.TEXCOORD;
    
    return output;
}

// Normal Mapping Function
float3 GetNormalFromMap(VS_OUTPUT input, float3 normalMap)
{
    // Expand normal from [0,1] to [-1,1]
    float3 tangentNormal = normalize(normalMap * 2.0 - 1.0);
    
    // Create TBN matrix
    float3x3 TBN = float3x3(
        normalize(input.Tangent),
        normalize(input.Bitangent),
        normalize(input.Normal)
    );
    
    return normalize(mul(tangentNormal, TBN));
}

// Improved Lighting Functions
float3 CalculateDirectional(DirectionalLight light, float3 normal, float3 viewDir, float3 worldPos)
{
    // Diffuse
    float3 lightDir = normalize(-light.Direction);
    float NdotL = max(dot(normal, lightDir), 0.0);
    float3 diffuse = light.Color * light.Intensity * NdotL;
    
    // Specular (Blinn-Phong)
    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), light.SpecularPower);
    float3 specular = light.Color * light.Intensity * spec;
    
    return diffuse + specular;
}

float3 CalculatePoint(PointLight light, float3 normal, float3 viewDir, float3 worldPos)
{
    float3 lightVec = light.Position - worldPos;
    float distance = length(lightVec);
    float3 lightDir = lightVec / distance;
    
    // Attenuation
    float attenuation = 1.0 / (1.0 + light.Attenuation * (distance * distance));
    attenuation *= smoothstep(light.Range, light.Range * 0.8, distance);
    
    // Diffuse
    float NdotL = max(dot(normal, lightDir), 0.0);
    float3 diffuse = light.Color * light.Intensity * NdotL;
    
    // Specular
    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), light.SpecularPower);
    float3 specular = light.Color * light.Intensity * spec;
    
    return (diffuse + specular) * attenuation;
}

float3 CalculateSpot(SpotLight light, float3 normal, float3 viewDir, float3 worldPos)
{
    float3 lightVec = light.Position - worldPos;
    float distance = length(lightVec);
    float3 lightDir = lightVec / distance;
    
    // Spot factor
    float spotFactor = dot(lightDir, normalize(-light.Direction));
    float edgeSmooth = 1.0 - smoothstep(light.ConeAngle * 0.9, light.ConeAngle, acos(spotFactor)));
    
    // Combine with point light calculations
    float3 pointLight = CalculatePoint(light, normal, viewDir, worldPos);
    return pointLight * edgeSmooth;
}

// Pixel Shader
float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    // Sample textures
    float4 totalTexture = float4(1,1,1,1);

    float4 diffuseSample = DiffuseTexture.Sample(diffuseState, input.TexCoord);
    float4 normalSample = NormalTexture.Sample(normalState, input.TexCoord);
    float4 specularSample = SpecularTexture.Sample(specularState, input.TexCoord);
    float4 metalicSample = metalicTexture.Sample(metalicState, input.TexCoord);
    float4 roughnessSample = roughnessTexture.Sample(roughnessState, input.TexCoord);
    float4 AOSample = AOTexture.Sample(AOState, input.TexCoord);
    
    if(HasDiffuse)
        totalTexture *= diffuseSample;

    // Get surface properties
    float3 normal = GetNormalFromMap(input, normalSample.rgb);
    float3 viewDir = normalize(input.WorldPos - GetCameraPosition());
    
    // Initialize lighting
    float3 lighting = AmbientColor * AmbientIntensity;
    
    // Process lights
    for(int i = 0; i < numDirectionalLights; i++)
        lighting += CalculateDirectional(directionalLights[i], normal, viewDir, input.WorldPos);
    
    for(int j = 0; j < numPointLights; j++)
        lighting += CalculatePoint(pointLights[j], normal, viewDir, input.WorldPos);
    
    for(int k = 0; k < numSpotLights; k++)
        lighting += CalculateSpot(spotLights[k], normal, viewDir, input.WorldPos);
    
    // Combine components
    float3 finalColor = diffuseSample.rgb * lighting + specularSample.rgb * lighting;
    
    // Gamma correction
    finalColor = pow(finalColor, 1.0/2.2);
    
    return float4(finalColor, diffuseSample.a);
}