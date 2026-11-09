#version 450 core

// Shadow depth pass, OpenGL: default Vertex layout, skinned via binding 3

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;
layout(location = 5) in ivec4 inBoneIDs;
layout(location = 6) in vec4 inBoneWeights;

// LightVP * World in WVP during the shadow pass
layout(std140, binding = 0) uniform TransformUBO {
    mat4 WVP;
    mat4 World;
};

// Bone palette, shared layout with skinned_shader_gl.vert
const int MAX_BONES = 256;
layout(std140, binding = 3) uniform BoneUBO {
    mat4 boneMatrices[MAX_BONES];
};

// Total bone weight below which a vertex counts as unskinned
const float SKIN_WEIGHT_EPSILON = 0.01;
const int BONES_PER_VERTEX = 4;

void main() {
    float totalWeight = inBoneWeights[0] + inBoneWeights[1] +
                        inBoneWeights[2] + inBoneWeights[3];

    vec4 skinnedPos;
    if (totalWeight > SKIN_WEIGHT_EPSILON) {
        mat4 skinMatrix = mat4(0.0);
        for (int i = 0; i < BONES_PER_VERTEX; i++) {
            if (inBoneIDs[i] >= 0 && inBoneIDs[i] < MAX_BONES)
                skinMatrix += boneMatrices[inBoneIDs[i]] * inBoneWeights[i];
        }
        skinnedPos = skinMatrix * vec4(inPosition, 1.0);
    } else {
        skinnedPos = vec4(inPosition, 1.0);
    }

    gl_Position = WVP * skinnedPos;
}
