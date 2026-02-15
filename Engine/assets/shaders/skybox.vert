#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;

layout(push_constant) uniform SkyboxPC {
    mat4 ViewProjection;
};

layout(location = 0) out vec3 fragTexCoord;

void main() {
    fragTexCoord = inPosition;
    vec4 pos = ViewProjection * vec4(inPosition, 1.0);
    pos.y = -pos.y;  // Vulkan Y-axis flip
    gl_Position = pos.xyww;
}
