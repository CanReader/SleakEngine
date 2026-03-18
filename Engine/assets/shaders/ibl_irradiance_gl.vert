#version 450 core

layout(location = 0) in vec3 inPosition;

layout(std140, binding = 0) uniform FaceUBO {
    mat4 ViewProjection;
};

out vec3 fragLocalPos;

void main() {
    fragLocalPos = inPosition;
    gl_Position = ViewProjection * vec4(inPosition, 1.0);
}
