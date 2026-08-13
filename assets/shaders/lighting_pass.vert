#version 450

// ============================================================
// Deferred Lighting Pass - Vulkan Vertex Shader
// Generates a full-screen triangle from gl_VertexID (no VBO needed).
// gl_VertexID:  0 -> (-1,-1)  1 -> (3,-1)  2 -> (-1, 3)
// ============================================================

layout(location = 0) out vec2 fragUV;

void main() {
    // Vertices for a single large triangle covering the screen
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 uvs[3] = vec2[](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0)
    );

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragUV = uvs[gl_VertexIndex];
}
