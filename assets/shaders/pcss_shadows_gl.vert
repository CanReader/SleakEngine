#version 450 core

// PCSS Shadow Debug Visualization Vertex Shader (OpenGL)
// Fullscreen triangle — no vertex buffer needed

out vec2 fragUV;

void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    fragUV = uv;
}
