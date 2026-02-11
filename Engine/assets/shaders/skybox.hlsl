TextureCube skyboxTexture : register(t0);
SamplerState samplerState : register(s0);


cbuffer CameraBuffer : register(b0) {
    matrix viewMatrix;
    matrix projectionMatrix;
}

struct VSInput {
    float3 position : POSITION;
};

struct PSInput {
    float3 texCoord : TEXCOORD;
};

PSInput main(VSInput input) {
    PSInput output;

    // Transform the vertex position into view space
    float4 viewPos = mul(float4(input.position, 1.0), viewMatrix);

    // Pass the position as the texture coordinate for cubemap sampling
    output.texCoord = viewPos.xyz;

    return output;
}

struct PSInput {
    float3 texCoord : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET {
    // Sample the cubemap using the direction vector
    return skyboxTexture.Sample(samplerState, input.texCoord);
}