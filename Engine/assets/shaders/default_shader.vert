#version 450

// ============================================================
// DXCraft Basic Material Shader - Vulkan Vertex Shader
// ============================================================

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;

// Transform push constant
layout(push_constant) uniform TransformPC {
    mat4 WVP;
    mat4 World;
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragWorldNorm;
layout(location = 2) out vec3 fragWorldTan;
layout(location = 3) out vec3 fragWorldBit;
layout(location = 4) out vec4 fragColor;
layout(location = 5) out vec2 fragUV;

void main() {
    gl_Position = WVP * vec4(inPosition, 1.0);
    gl_Position.y = -gl_Position.y;  // Vulkan Y-axis flip

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
