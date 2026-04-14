#version 450

// Shadow Depth Pass - Voxel Vertex Only
// Compact VoxelVertex layout: only position needed for shadow map

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inUV;

// Push constant: LightVP*World in slot 0, World in slot 1
layout(push_constant) uniform TransformPC {
    mat4 WVP;
    mat4 World;
};

void main() {
    gl_Position = WVP * vec4(inPosition, 1.0);
}
