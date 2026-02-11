#pragma once

#include "../../include/private/Graphics/Vertex.hpp"
#include <Math/Math.hpp>
#include <Math/Vector.hpp>

inline Sleak::MeshData GetPlaneMesh(float width, float height, int subdivisionsX = 1, int subdivisionsY = 1) {
    Sleak::MeshData mesh;

    // Calculate step sizes for subdivisions
    float stepX = width / subdivisionsX;
    float stepY = height / subdivisionsY;

    // Generate vertices
    for (int y = 0; y <= subdivisionsY; ++y) {
        for (int x = 0; x <= subdivisionsX; ++x) {
            float posX = -width / 2.0f + x * stepX;
            float posY = 0.0f;                     
            float posZ = -height / 2.0f + y * stepY;

            float u = x / (float)subdivisionsX;
            float v = y / (float)subdivisionsY;

            mesh.vertices.AddVertex(Sleak::Vertex(
                posX, posY, posZ,  // Position
                0, 1, 0,           // Normal (pointing up)
                1, 0, 0,           // Tangent
                u, v               // UV
            ));
        }
    }

    for (int y = 0; y < subdivisionsY; ++y) {
        for (int x = 0; x < subdivisionsX; ++x) {
            int topLeft = y * (subdivisionsX + 1) + x;
            int topRight = topLeft + 1;
            int bottomLeft = (y + 1) * (subdivisionsX + 1) + x;
            int bottomRight = bottomLeft + 1;

            // First triangle
            mesh.indices.add(topLeft);
            mesh.indices.add(bottomLeft);
            mesh.indices.add(topRight);

            // Second triangle
            mesh.indices.add(topRight);
            mesh.indices.add(bottomLeft);
            mesh.indices.add(bottomRight);
        }
    }

    return mesh;
}

inline Sleak::MeshData GetCubeMesh() {
    Sleak::MeshData mesh;
                                          //Position        | Normal | Tangent    | UV
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, -0.5f, 0.5f, 0, 0, 1, 1, 1, 1, 1, 0, 1));  // 0: bottom-left
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, +0.5f, 0.5f, 0, 0, 1, 1, 1, 1, 1, 0, 0));  // 1: top-left
    mesh.vertices.AddVertex(Sleak::Vertex(+0.5f, +0.5f, 0.5f, 0, 0, 1, 1, 1, 1, 1, 1, 0));  // 2: top-right
    mesh.vertices.AddVertex(Sleak::Vertex(+0.5f, -0.5f, 0.5f, 0, 0, 1, 1, 1, 1, 1, 1, 1));  // 3: bottom-right

    // Back face (-Z)
                                          //Position         | Normal | Tangent    | UV
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, -0.5f, -0.5f, 0, 0, -1, 1, 1, 1, 1, 0, 1));  // 4: bottom-left
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, 0.5f, -0.5f, 0, 0, -1, 1, 1, 1, 1, 0, 0));  // 5: top-left
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, 0.5f, -0.5f, 0, 0, -1, 1, 1, 1, 1, 1, 0));  // 6: top-right
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, -0.5f, -0.5f, 0, 0, -1, 1, 1, 1, 1, 1, 1));  // 7: bottom-right

    // Left face (-X)
                                          //Position         | Normal | Tangent    | UV
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, -0.5f, -0.5f, -1, 0, 0, 1, 1, 1, 1, 0, 1));  // 8: bottom-left
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, 0.5f, -0.5f, -1, 0, 0, 1, 1, 1, 1, 0, 0));  // 9: top-left
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, 0.5f, 0.5f, -1, 0, 0, 1, 1, 1, 1, 1, 0));  // 10: top-right
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, -0.5f, 0.5f, -1, 0, 0, 1, 1, 1, 1, 1, 1));  // 11: bottom-right

    // Right face (+X)
                                          //Position         | Normal | Tangent    | UV
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, -0.5f, 0.5f, 1, 0, 0, 1, 1, 1, 1, 0, 1));  // 12: bottom-left
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, 0.5f, 0.5f, 1, 0, 0, 1, 1, 1, 1, 0, 0));  // 13: top-left
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, 0.5f, -0.5f, 1, 0, 0, 1, 1, 1, 1, 1, 0));  // 14: top-right
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, -0.5f, -0.5f, 1, 0, 0, 1, 1, 1, 1, 1, 1));  // 15: bottom-right

    // Top face (+Y)
                                          //Position         | Normal | Tangent    | UV
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, 0.5f, 0.5f, 0, 1, 0, 0, 1, 1, 1, 0, 1));  // 16: bottom-left
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, 0.5f, -0.5f, 0, 1, 0, 1, 1, 1, 1, 0, 0));  // 17: top-left
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, 0.5f, -0.5f, 0, 1, 0, 1, 1, 1, 1, 1, 0));  // 18: top-right
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, 0.5f, 0.5f, 0, 1, 0, 1, 1, 0, 1, 1, 1));  // 19: bottom-right

    // Bottom face (-Y)
                                          //Position         | Normal | Tangent    | UV
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, -0.5f, -0.5f, 0, -1, 0, 1, 1, 1, 1, 0, 1));  // 20: bottom-left
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, -0.5f, 0.5f, 0, -1, 0, 1, 1, 1, 1, 0, 0));  // 21: top-left
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, -0.5f, 0.5f, 0, -1, 0, 1, 1, 1, 1, 1, 0));  // 22: top-right
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, -0.5f, -0.5f, 0, -1, 0, 1, 1, 1, 1, 1, 1));  // 23: bottom-right

    mesh.indices = {// Front face
                    0, 1, 2, 0, 2, 3,
                    // Back face
                    4, 5, 6, 4, 6, 7,
                    // Left face
                    8, 9, 10, 8, 10, 11,
                    // Right face
                    12, 13, 14, 12, 14, 15,
                    // Top face
                    16, 17, 18, 16, 18, 19,
                    // Bottom face
                    20, 21, 22, 20, 22, 23};
    return mesh;
}

inline Sleak::MeshData GetSphereMesh(int stacks = 16, int slices = 16) {
    Sleak::MeshData mesh;
    
    for (int i = 0; i <= stacks; ++i) {
        float phi = PI * i / stacks;
        for (int j = 0; j <= slices; ++j) {
            float theta = 2 * PI * j / slices;
            float x = sin(phi) * cos(theta);
            float y = cos(phi);
            float z = sin(phi) * sin(theta);
            
            mesh.vertices.AddVertex(Sleak::Vertex(x, y, z, // position
                                           x, y, z, //Normal
                                          -sin(theta), 0, cos(theta), // Tangent
                                          (float)j / slices, (float)i / stacks)); //UV
        }
    }
    
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            int first = i * (slices + 1) + j;
            int second = first + slices + 1;
            mesh.indices.add(first);
            mesh.indices.add(second);
            mesh.indices.add(first + 1);
            mesh.indices.add(second);
            mesh.indices.add(second + 1);
            mesh.indices.add(first + 1);
        }
    }
    return mesh;
}

inline Sleak::MeshData GetCylinderMesh(int segments, float height, float radius) {
    Sleak::MeshData mesh;

    // Generate side vertices (centered at origin)
    for (int i = 0; i <= segments; ++i) {
        float theta = 2.0f * PI * i / segments;
        float x = cos(theta) * radius;
        float z = sin(theta) * radius;

        // Side vertices (with proper normals)
        mesh.vertices.AddVertex(Sleak::Vertex(
            x, -height/2, z,  // Bottom vertex
            cos(theta), 0, sin(theta),  // Side normal
            -sin(theta), 0, cos(theta),  // Tangent
            i/(float)segments, 0
        ));

        mesh.vertices.AddVertex(Sleak::Vertex(
            x, height/2, z,  // Top vertex
            cos(theta), 0, sin(theta),  // Side normal
            -sin(theta), 0, cos(theta),  // Tangent
            i/(float)segments, 1
        ));
    }

    // Center vertices for caps
    mesh.vertices.AddVertex(Sleak::Vertex(
        0, -height/2, 0, 0, -1, 0, 1, 0, 0, 0.5f, 0.5f
    ));
    int bottomCenter = mesh.vertices.GetSize() - 1;

    mesh.vertices.AddVertex(Sleak::Vertex(
        0, height/2, 0, 0, 1, 0, 1, 0, 0, 0.5f, 0.5f
    ));
    int topCenter = mesh.vertices.GetSize() - 1;

    // Generate indices
    for (int i = 0; i < segments; ++i) {
        int base = i * 2;
        
        // Side quads
        mesh.indices.add(base);
        mesh.indices.add(base + 1);
        mesh.indices.add(base + 2);
        
        mesh.indices.add(base + 1);
        mesh.indices.add(base + 3);
        mesh.indices.add(base + 2);

        // Bottom cap
        mesh.indices.add(base);
        mesh.indices.add(base + 2);
        mesh.indices.add(bottomCenter);

        // Top cap
        mesh.indices.add(base + 1);
        mesh.indices.add(topCenter);
        mesh.indices.add(base + 3);
    }

    return mesh;
}

inline Sleak::MeshData GetCapsuleMesh(int segments, int rings, float height, float radius) {
    Sleak::MeshData mesh;
    float halfHeight = height * 0.5f;

    // Generate vertices
    for (int i = 0; i <= rings; ++i) {
        float phi = PI * i / rings; // Now from 0 to PI (top to bottom)
        for (int j = 0; j <= segments; ++j) {
            float theta = 2.0f * PI * j / segments;
            float x = sin(phi) * cos(theta) * radius;
            float y = cos(phi) * radius;
            float z = sin(phi) * sin(theta) * radius;

            // Adjust y position for hemispheres
            if (i <= rings/2) {
                y -= halfHeight; // Bottom hemisphere
            } else {
                y += halfHeight; // Top hemisphere
            }

            // Correct normal calculation
            Sleak::Math::Vector3D normal = Sleak::Math::Vector3D(
                sin(phi) * cos(theta),
                cos(phi),
                sin(phi) * sin(theta)
            ).Normalize();

            mesh.vertices.AddVertex(Sleak::Vertex(
                x, y, z,
                normal.GetX(), normal.GetY(), normal.GetZ(),
                -sin(theta), 0, cos(theta), // Correct tangent
                j / (float)segments, i / (float)rings
            ));
        }
    }

    // Generate indices (fixed winding order)
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            int first = i * (segments + 1) + j;
            int second = first + segments + 1;

            mesh.indices.add(first);
            mesh.indices.add(second);
            mesh.indices.add(first + 1);

            mesh.indices.add(second);
            mesh.indices.add(second + 1);
            mesh.indices.add(first + 1);
        }
    }

    return mesh;
}

inline Sleak::MeshData GetTorusMesh(int segments, int rings, float innerRadius, float outerRadius) {
    Sleak::MeshData mesh;

    float radius = (outerRadius - innerRadius) / 2.0f;
    float center = innerRadius + radius;

    // Generate vertices
    for (int i = 0; i <= rings; ++i) {
        float phi = 2.0f * PI * i / rings; // Ring angle
        for (int j = 0; j <= segments; ++j) {
            float theta = 2.0f * PI * j / segments; // Segment angle
            float x = (center + radius * cos(theta)) * cos(phi);
            float y = radius * sin(theta);
            float z = (center + radius * cos(theta)) * sin(phi);

            mesh.vertices.AddVertex(Sleak::Vertex(
                x, y, z,  // Position
                cos(theta) * cos(phi), sin(theta), cos(theta) * sin(phi), // Normal
                1, 0, 0,  // Tangent
                j / (float)segments, i / (float)rings // UV
            ));
        }
    }

    // Generate indices
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            int first = i * (segments + 1) + j;
            int second = first + segments + 1;

            // First triangle
            mesh.indices.add(first);
            mesh.indices.add(second);
            mesh.indices.add(first + 1);

            // Second triangle
            mesh.indices.add(second);
            mesh.indices.add(second + 1);
            mesh.indices.add(first + 1);
        }
    }

    return mesh;
}

inline Sleak::MeshData GetPyramidMesh() {
    Sleak::MeshData mesh;

    // Vertices
    mesh.vertices.AddVertex(Sleak::Vertex(0, 0.5f, 0, 0, 1, 0, 1, 0, 0, 0, 0.5f, 1)); // Apex
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, -0.5f, 0.5f, 0, -1, 1, 1, 0, 0, 0, 0, 0)); // Bottom-left
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, -0.5f, 0.5f, 0, -1, 1, 1, 0, 0, 0, 1, 0)); // Bottom-right
    mesh.vertices.AddVertex(Sleak::Vertex(0.5f, -0.5f, -0.5f, 0, -1, 1, 1, 0, 0, 0, 1, 1)); // Back-right
    mesh.vertices.AddVertex(Sleak::Vertex(-0.5f, -0.5f, -0.5f, 0, -1, 1, 1, 0, 0, 0, 0, 1)); // Back-left

    // Indices
    // Base
    mesh.indices.add(1);
    mesh.indices.add(2);
    mesh.indices.add(3);
    mesh.indices.add(1);
    mesh.indices.add(3);
    mesh.indices.add(4);

    // Faces
    mesh.indices.add(0);
    mesh.indices.add(1);
    mesh.indices.add(2);

    mesh.indices.add(0);
    mesh.indices.add(2);
    mesh.indices.add(3);

    mesh.indices.add(0);
    mesh.indices.add(3);
    mesh.indices.add(4);

    mesh.indices.add(0);
    mesh.indices.add(4);
    mesh.indices.add(1);

    return mesh;
}
