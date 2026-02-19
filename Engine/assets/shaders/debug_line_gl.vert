#version 450 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUV;

layout(std140, binding = 0) uniform DebugLineUBO {
    mat4 ViewProjection;
};

out vec4 fragColor;

void main() {
    fragColor = inColor;
    gl_Position = ViewProjection * vec4(inPosition, 1.0);
}
