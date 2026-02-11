#version 450 core

// ============================================================
// DXCraft Basic Material Shader - OpenGL Vertex Shader
// ============================================================

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;

// Transform UBO (binding 0) - matches TransformBuffer layout
layout(std140, binding = 0) uniform TransformUBO {
    mat4 WVP;
    mat4 World;
};

out vec3 fragWorldPos;
out vec3 fragWorldNorm;
out vec3 fragWorldTan;
out vec3 fragWorldBit;
out vec4 fragColor;
out vec2 fragUV;

void main() {
    gl_Position = WVP * vec4(inPosition, 1.0);

    // World-space position and basis vectors for lighting
    vec4 worldPos = World * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;

    mat3 worldMat3 = mat3(World);
    fragWorldNorm = normalize(worldMat3 * inNormal);
    fragWorldTan  = normalize(worldMat3 * inTangent.xyz);
    fragWorldBit  = cross(fragWorldNorm, fragWorldTan)
                    * inTangent.w;

    fragColor = inColor;
    fragUV    = inUV;
}
