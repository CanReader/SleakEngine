#version 450 core

// Fullscreen triangle vertex shader — no vertex buffer needed
// Uses gl_VertexID to generate 3 vertices covering the screen

out vec2 fragUV;

void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    fragUV = uv;
}
