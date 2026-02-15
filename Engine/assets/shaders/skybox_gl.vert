#version 450 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;

// Skybox UBO (binding 0) - ViewNoTranslation * Projection
layout(std140, binding = 0) uniform SkyboxUBO {
    mat4 ViewProjection;
};

out vec3 fragTexCoord;

void main() {
    fragTexCoord = inPosition;
    vec4 pos = ViewProjection * vec4(inPosition, 1.0);
    // Force depth to maximum (1.0) so skybox is always behind everything
    gl_Position = pos.xyww;
}
