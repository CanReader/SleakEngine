#version 450 core

// ============================================================
// Screen Space Ambient Occlusion (SSAO) - OpenGL Fragment
// ============================================================

in vec2 fragUV;
out float outColor;

layout(std140, binding = 0) uniform SSAOUBO {
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

layout(std140, binding = 1) uniform KernelUBO {
    vec4 ssaoKernel[64];
};

layout(std140, binding = 2) uniform CameraUBO {
    mat4 Projection;
    mat4 View;
    mat4 InvProjection;
};

layout(binding = 0) uniform sampler2D depthTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D noiseTexture;

vec3 ReconstructViewPos(vec2 uv, float depth) {
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = InvProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

void main() {
    if (ssaoEnabled == 0u) {
        outColor = 1.0;
        return;
    }

    float depth = texture(depthTexture, fragUV).r;
    if (depth >= 1.0) {
        outColor = 1.0;
        return;
    }

    vec3 fragPos = ReconstructViewPos(fragUV, depth);

    // GBuffer stores world-space normal packed to [0,1]. Transform to view
    // space for SSAO's view-space hemisphere kernel to be oriented correctly.
    vec3 worldN = normalize(texture(normalTexture, fragUV).rgb * 2.0 - 1.0);
    vec3 normal = normalize((View * vec4(worldN, 0.0)).xyz);

    vec2 noiseScale = vec2(screenWidth / 4.0, screenHeight / 4.0);
    vec3 randomVec = texture(noiseTexture, fragUV * noiseScale).xyz;

    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;

    for (uint i = 0u; i < ssaoKernelSize; ++i) {
        vec3 sampleDir = TBN * ssaoKernel[i].xyz;
        vec3 samplePos = fragPos + sampleDir * ssaoRadius;

        vec4 offset = Projection * vec4(samplePos, 1.0);
        offset.xy /= offset.w;
        offset.xy = offset.xy * 0.5 + 0.5;

        float sampleDepth = texture(depthTexture, offset.xy).r;
        vec3 sampleViewPos = ReconstructViewPos(offset.xy, sampleDepth);

        float rangeCheck = smoothstep(0.0, 1.0,
            ssaoRadius / abs(fragPos.z - sampleViewPos.z));
        occlusion += (sampleViewPos.z >= samplePos.z + ssaoBias ? 1.0 : 0.0)
                   * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(ssaoKernelSize));
    occlusion = pow(occlusion, ssaoPower);

    outColor = occlusion;
}
