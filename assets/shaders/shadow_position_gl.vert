#version 450 core

// Shadow depth pass, OpenGL: position-only caster for custom vertex formats
layout(location = 0) in vec3 inPosition;

// LightVP * World in WVP during the shadow pass
layout(std140, binding = 0) uniform TransformUBO {
    mat4 WVP;
    mat4 World;
};

void main() {
    gl_Position = WVP * vec4(inPosition, 1.0);
}
