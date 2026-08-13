#version 450 core

// Fullscreen triangle — no vertex buffer required.
// Call glDrawArrays(GL_TRIANGLES, 0, 3) with an empty VAO.
out vec2 fragUV;

void main() {
    // Vertex positions for a triangle that covers the entire screen:
    //   0: (-1,-1)  1: (3,-1)  2: (-1, 3)
    vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 pos = positions[gl_VertexID];
    gl_Position = vec4(pos, 0.0, 1.0);
    fragUV = pos * 0.5 + 0.5;
}
