#version 450 core

// ============================================================
// Screen Space Ambient Occlusion (SSAO) - OpenGL Fragment
//
// Works in WORLD space. World position is reconstructed from the depth
// buffer via InvViewProj (no worldpos GBuffer RT), matching the tap the
// lighting pass uses.
// ============================================================

in vec2 fragUV;
out float outColor;

layout(std140, binding = 7) uniform SSAOUBO {
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

layout(std140, binding = 8) uniform KernelUBO {
    vec4 ssaoKernel[64];
};

layout(std140, binding = 9) uniform CameraUBO {
    mat4 Projection;
    mat4 View;
    mat4 InvViewProj;     // clip -> world (depth reconstruction)
};

layout(binding = 0) uniform sampler2D depthTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D noiseTexture;

// Reconstruct world position from depth. OpenGL: no vertex Y-flip, and the
// default clip depth range is [-1,1], so the stored [0,1] depth maps to
// NDC.z = 2*depth-1 before InvViewProj.
vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc   = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = InvViewProj * ndc;
    return world.xyz / world.w;
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

    vec3 worldPos = ReconstructWorldPos(fragUV, depth);
    vec3 worldN   = normalize(texture(normalTexture, fragUV).rgb * 2.0 - 1.0);

    vec2 noiseScale = vec2(screenWidth / 4.0, screenHeight / 4.0);
    vec3 randomVec  = texture(noiseTexture, fragUV * noiseScale).xyz;

    // TBN built directly around the world-space normal — kernel samples
    // are then placed in world space.
    vec3 tangent   = normalize(randomVec - worldN * dot(randomVec, worldN));
    vec3 bitangent = cross(worldN, tangent);
    mat3 TBN       = mat3(tangent, bitangent, worldN);

    // Reference view-space Z for the current fragment.
    // View * worldPos here only needs to be consistent with itself — the
    // comparison is self-referential, so even if the view matrix is
    // column- vs row-major mis-interpreted, both sides of the compare are
    // transformed the same way and the result stays meaningful.
    float fragViewZ = (View * vec4(worldPos, 1.0)).z;

    mat4 VP = Projection * View;

    float occlusion = 0.0;

    for (uint i = 0u; i < ssaoKernelSize; ++i) {
        vec3 sampleWorld = worldPos + (TBN * ssaoKernel[i].xyz) * ssaoRadius;

        vec4 clipSample = VP * vec4(sampleWorld, 1.0);
        if (clipSample.w <= 0.0) continue;

        vec2 uv = clipSample.xy / clipSample.w * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

        float sampleDepth = texture(depthTexture, uv).r;
        if (sampleDepth >= 1.0) continue;  // sky — no occluder
        vec3  actualWorld = ReconstructWorldPos(uv, sampleDepth);
        float actualViewZ = (View * vec4(actualWorld, 1.0)).z;
        float sampleViewZ = (View * vec4(sampleWorld, 1.0)).z;

        float rangeCheck = smoothstep(0.0, 1.0,
            ssaoRadius / max(abs(fragViewZ - actualViewZ), 1e-4));

        // Occluded when real geometry at this UV is closer to camera than
        // the sampled hemisphere point. "Closer to camera" is the larger
        // (less-negative in GL) view-space Z.
        occlusion += (actualViewZ >= sampleViewZ + ssaoBias ? 1.0 : 0.0)
                   * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(ssaoKernelSize));
    outColor = pow(occlusion, ssaoPower);
}
