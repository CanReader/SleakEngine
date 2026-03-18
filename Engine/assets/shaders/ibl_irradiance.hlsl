// ============================================================
// IBL Irradiance Convolution Shader (DirectX 11 HLSL)
// Convolves environment cubemap into diffuse irradiance map
// ============================================================

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float3 LocalPos : TEXCOORD0;
};

cbuffer FaceCB : register(b0) {
    row_major float4x4 ViewProjection;
};

// Cube vertices are passed as vertex buffer
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

float4 PS_Main(VS_OUTPUT input) : SV_Target
{
    float3 N = normalize(input.LocalPos);

    float3 irradiance = float3(0.0, 0.0, 0.0);

    // Tangent-space hemisphere sampling
    float3 up    = float3(0.0, 1.0, 0.0);
    float3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    float sampleDelta = 0.025;
    float nrSamples = 0.0;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            // Spherical to cartesian (tangent space)
            float3 tangentSample = float3(
                sin(theta) * cos(phi),
                sin(theta) * sin(phi),
                cos(theta));

            // Tangent to world
            float3 sampleVec = tangentSample.x * right
                             + tangentSample.y * up
                             + tangentSample.z * N;

            irradiance += environmentMap.Sample(envSampler, sampleVec).rgb
                        * cos(theta) * sin(theta);
            nrSamples++;
        }
    }

    irradiance = PI * irradiance * (1.0 / nrSamples);

    return float4(irradiance, 1.0);
}
