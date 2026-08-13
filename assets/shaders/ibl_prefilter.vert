#version 450

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform PrefilterPC {
    mat4 ViewProjection;
    float Roughness;
};

layout(location = 0) out vec3 fragLocalPos;

void main() {
    fragLocalPos = inPosition;
    gl_Position = ViewProjection * vec4(inPosition, 1.0);
}
