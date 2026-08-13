#version 450 core

in vec3 fragTexCoord;
out vec4 outColor;

layout(binding = 0) uniform samplerCube skyboxTexture;

// ACES tone mapping — needed for HDR skyboxes whose pixel values exceed 1.0.
// LDR JPG inputs already lie in [0,1] so this is a near no-op for them.
vec3 ACESFilm(vec3 x) {
    return clamp((x * (2.51 * x + 0.03))
               / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(skyboxTexture, fragTexCoord).rgb;
    outColor = vec4(ACESFilm(hdr), 1.0);
}
