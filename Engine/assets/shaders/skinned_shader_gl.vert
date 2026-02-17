#version 450 core

// ============================================================
// Skinned Material Shader - OpenGL Vertex Shader
// ============================================================

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;
layout(location = 5) in ivec4 inBoneIDs;
layout(location = 6) in vec4 inBoneWeights;

// Transform UBO (binding 0) - matches TransformBuffer layout
layout(std140, binding = 0) uniform TransformUBO {
    mat4 WVP;
    mat4 World;
};

// Bone matrices UBO (binding 3)
const int MAX_BONES = 256;
layout(std140, binding = 3) uniform BoneUBO {
    mat4 boneMatrices[MAX_BONES];
};

out vec3 fragWorldPos;
out vec3 fragWorldNorm;
out vec3 fragWorldTan;
out vec3 fragWorldBit;
out vec4 fragColor;
out vec2 fragUV;

void main() {
    // Compute skinned position
    float totalWeight = inBoneWeights[0] + inBoneWeights[1] +
                        inBoneWeights[2] + inBoneWeights[3];

    mat4 skinMatrix;
    if (totalWeight > 0.01) {
        skinMatrix = mat4(0.0);
        for (int i = 0; i < 4; i++) {
            if (inBoneIDs[i] >= 0 && inBoneIDs[i] < MAX_BONES)
                skinMatrix += boneMatrices[inBoneIDs[i]] * inBoneWeights[i];
        }
    } else {
        skinMatrix = mat4(1.0);
    }

    vec4 skinnedPos = skinMatrix * vec4(inPosition, 1.0);
    gl_Position = WVP * skinnedPos;

    // World-space position and basis vectors for lighting
    vec4 worldPos = World * skinnedPos;
    fragWorldPos = worldPos.xyz;

    mat3 skinMat3 = mat3(skinMatrix);
    mat3 worldMat3 = mat3(World);
    mat3 finalNormalMat = worldMat3 * skinMat3;

    fragWorldNorm = normalize(finalNormalMat * inNormal);
    fragWorldTan  = normalize(finalNormalMat * inTangent.xyz);
    fragWorldBit  = cross(fragWorldNorm, fragWorldTan) * inTangent.w;

    fragColor = inColor;
    fragUV    = inUV;
}
