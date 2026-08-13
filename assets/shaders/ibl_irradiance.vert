#version 450

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform FacePC {
    mat4 ViewProjection;
};

layout(location = 0) out vec3 fragLocalPos;

void main() {
    fragLocalPos = inPosition;
    gl_Position = ViewProjection * vec4(inPosition, 1.0);
}
