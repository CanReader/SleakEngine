#version 450

// ============================================================
// GBuffer Geometry Pass - Vulkan Vertex Shader
// Outputs world-space data for deferred shading G-Buffer fill
// ============================================================

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;
layout(location = 5) in ivec4 inBoneIDs;
layout(location = 6) in vec4  inBoneWeights;

// Transform push constant (same as default_shader.vert)
layout(push_constant) uniform TransformPC {
    mat4 WVP;
    mat4 World;
};

// Light/Shadow UBO (set 2, binding 0) — needed for shadow coordinate
layout(set = 2, binding = 0) uniform ShadowLightUBO {
    vec4  uLightDir;
    vec4  uLightColor;
    vec4  uAmbient;
    vec4  uCameraPos;
    mat4  uLightVP;
    float uShadowBias;
    float uShadowStrength;
    float uShadowTexelSize;
    float uLightSize;
    vec4  uFogColor;
    float uFogStart;
    float uFogEnd;
    vec2  _fogPad;            // std140: must be vec2 (8B) not float[2] (stride-16 = 32B)
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragWorldNorm;
layout(location = 2) out vec3 fragWorldTan;
layout(location = 3) out vec3 fragWorldBit;
layout(location = 4) out vec4 fragColor;
layout(location = 5) out vec2 fragUV;
layout(location = 6) out vec4 fragShadowCoord;

void main() {
    gl_Position = WVP * vec4(inPosition, 1.0);
    gl_Position.y = -gl_Position.y;  // Vulkan Y-axis flip

    vec4 worldPos = World * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;

    mat3 worldMat3 = mat3(World);
    fragWorldNorm = normalize(worldMat3 * inNormal);
    fragWorldTan  = normalize(worldMat3 * inTangent.xyz);
    fragWorldBit  = cross(fragWorldNorm, fragWorldTan) * inTangent.w;

    fragColor = inColor;
    fragUV    = inUV;

    float normalBias = uLightDir.w;
    vec4 biasedWorldPos = worldPos + vec4(fragWorldNorm * normalBias, 0.0);
    fragShadowCoord = uLightVP * biasedWorldPos;
}
