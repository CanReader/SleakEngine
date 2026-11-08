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

// View-space z from depth; equals (View * ReconstructWorldPos(uv, depth)).z
// because the projection's z and w rows depend on view z alone.
float LinearizeViewZ(float depth) {
    float ndcZ = depth * 2.0 - 1.0;
    return (Projection[3][2] - Projection[3][3] * ndcZ)
         / (Projection[2][3] * ndcZ - Projection[2][2]);
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

    // Reference view-space Z for the current fragment; View's z row maps a
    // world offset to its view-space z delta.
    float fragViewZ = LinearizeViewZ(depth);
    vec3  viewZAxis = vec3(View[0][2], View[1][2], View[2][2]);

    mat4 VP = Projection * View;

    float occlusion = 0.0;

    for (uint i = 0u; i < ssaoKernelSize; ++i) {
        vec3 sampleOffset = (TBN * ssaoKernel[i].xyz) * ssaoRadius;
        vec3 sampleWorld  = worldPos + sampleOffset;

        vec4 clipSample = VP * vec4(sampleWorld, 1.0);
        if (clipSample.w <= 0.0) continue;

        vec2 uv = clipSample.xy / clipSample.w * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

        float sampleDepth = texture(depthTexture, uv).r;
        if (sampleDepth >= 1.0) continue;  // sky — no occluder
        float actualViewZ = LinearizeViewZ(sampleDepth);
        float sampleViewZ = fragViewZ + dot(viewZAxis, sampleOffset);

        float rangeCheck = smoothstep(0.0, 1.0,
            ssaoRadius / max(abs(fragViewZ - actualViewZ), 1e-4));

        // Occluded when real geometry at this UV is closer to camera than
        // the sampled hemisphere point. The engine view is left-handed
        // (+Z forward), so closer means the smaller view-space Z.
        occlusion += (actualViewZ <= sampleViewZ - ssaoBias ? 1.0 : 0.0)
                   * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(ssaoKernelSize));
    outColor = pow(occlusion, ssaoPower);
}
