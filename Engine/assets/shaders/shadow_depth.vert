#version 450

// Shadow Depth Pass - Vertex Only
// Renders geometry from light's perspective to generate shadow map.
// Supports skinned meshes: when bone weights are non-zero, position is
// transformed by the weighted bone matrices before the light-space projection.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;
layout(location = 5) in ivec4 inBoneIDs;
layout(location = 6) in vec4 inBoneWeights;

// Push constant: LightVP*World in slot 0, World in slot 1
layout(push_constant) uniform TransformPC {
    mat4 WVP;
    mat4 World;
};

// Bone matrices UBO (set 1, binding 0) — shared layout with skinned_shader.vert
const int MAX_BONES = 256;
layout(set = 1, binding = 0) uniform BoneUBO {
    mat4 boneMatrices[MAX_BONES];
};

void main() {
    float totalWeight = inBoneWeights[0] + inBoneWeights[1] +
                        inBoneWeights[2] + inBoneWeights[3];

    vec4 skinnedPos;
    if (totalWeight > 0.01) {
        mat4 skinMatrix = mat4(0.0);
        for (int i = 0; i < 4; i++) {
            if (inBoneIDs[i] >= 0 && inBoneIDs[i] < MAX_BONES)
                skinMatrix += boneMatrices[inBoneIDs[i]] * inBoneWeights[i];
        }
        skinnedPos = skinMatrix * vec4(inPosition, 1.0);
    } else {
        skinnedPos = vec4(inPosition, 1.0);
    }

    gl_Position = WVP * skinnedPos;
}
