#version 450

// ============================================================
// Skinned Material Shader - Vulkan Vertex Shader
// Dynamic lighting via UBO + shadow coordinate output
// ============================================================

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;
layout(location = 5) in ivec4 inBoneIDs;
layout(location = 6) in vec4 inBoneWeights;

// Transform push constant
layout(push_constant) uniform TransformPC {
    mat4 WVP;
    mat4 World;
};

// Bone matrices UBO (set 1, binding 0)
const int MAX_BONES = 256;
layout(set = 1, binding = 0) uniform BoneUBO {
    mat4 boneMatrices[MAX_BONES];
};

// Light/Shadow UBO (set 2, binding 0)
layout(set = 2, binding = 0) uniform ShadowLightUBO {
    vec4  uLightDir;       // xyz = direction, w = pad
    vec4  uLightColor;     // rgb = color, a = intensity
    vec4  uAmbient;        // rgb = color, a = intensity
    vec4  uCameraPos;      // xyz = camera world pos, w = pad
    mat4  uLightVP;        // light view-projection matrix
    float uShadowBias;
    float uShadowStrength;
    float uShadowTexelSize;
    float _pad;
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragWorldNorm;
layout(location = 2) out vec3 fragWorldTan;
layout(location = 3) out vec3 fragWorldBit;
layout(location = 4) out vec4 fragColor;
layout(location = 5) out vec2 fragUV;
layout(location = 6) out vec4 fragShadowCoord;

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
    gl_Position.y = -gl_Position.y;  // Vulkan Y-axis flip

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

    // Shadow coordinate: offset along normal to reduce shadow acne,
    // then transform by light VP
    float normalBias = uLightDir.w;  // stored in LightDir.w
    vec4 biasedWorldPos = worldPos + vec4(fragWorldNorm * normalBias, 0.0);
    fragShadowCoord = uLightVP * biasedWorldPos;
}
