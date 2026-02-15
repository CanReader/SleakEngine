#version 450 core

in vec3 fragTexCoord;
out vec4 outColor;

layout(binding = 0) uniform samplerCube skyboxTexture;

void main() {
    outColor = texture(skyboxTexture, fragTexCoord);
}
