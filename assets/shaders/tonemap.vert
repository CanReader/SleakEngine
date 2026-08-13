#version 450

// Fullscreen triangle vertex shader for Vulkan post-process
// No vertex buffer — generates 3 vertices from gl_VertexIndex

layout(location = 0) out vec2 fragUV;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    fragUV = uv;
}
