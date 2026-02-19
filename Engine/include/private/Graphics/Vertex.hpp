#ifndef _VERTEX_HPP_
#define  _VERTEX_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <Utility/Container/List.hpp>

namespace Sleak {

    using IndexType = uint32_t;
    using IndexGroup = List<IndexType>;

    struct Vertex {
    // Position
    float px, py, pz;        // 12

    // Normal
    float nx, ny, nz;        // 12

    // Tangent
    float tx, ty, tz, tw;    // 16

    // Color
    float r, g, b, a;        // 16

    // UV
    float u, v;              // 8

    // Bone data (for skeletal animation)
    int boneIDs[4] = {-1, -1, -1, -1};    // 16 bytes
    float boneWeights[4] = {0, 0, 0, 0};  // 16 bytes
    // Total: 96 bytes

        // Constructor
        Vertex() = default;

        Vertex(float px, float py, float pz,
               float nx, float ny, float nz,
               float tx, float ty, float tz, float tw,
               float u = 0.0f, float v = 0.0f)
            : px(px), py(py), pz(pz),
              nx(nx), ny(ny), nz(nz),
              tx(tx), ty(ty), tz(tz), tw(tw),
              r(1.0f), g(1.0f), b(1.0f), a(1.0f),
              u(u), v(v) {
        }

        void SetPosition(float x, float y, float z) 
        {
            px = x;
            py = y;
            pz = z;
        }

         void SetNormal(float x, float y, float z) 
         {
            nx = x;
            ny = y;
            nz = z;
         }

         void SetColor(float r, float g, float b, float a) 
         {
             this->r = r;
             this->g = g;
             this->b = b;
             this->a = a;
         }

         void SetTexCoord(float u, float v) 
         { 
             this->u = u;
             this->v = v;
         }

        // Get the memory layout description for the GPU input layout
        static constexpr std::array<size_t, 6> GetAttributeSizes() {
            return { 3, 3, 4, 2, 4, 4 }; // Position, Normal, Color, TexCoord, BoneIDs(int), BoneWeights
        }

        // Get the total size of the vertex in bytes
        static constexpr size_t GetSize() {
            return sizeof(Vertex);
        }

        // Get the offset of each attribute in the vertex structure
        static constexpr std::array<size_t, 6> GetAttributeOffsets() {
            return {
                offsetof(Vertex, px),        // Position
                offsetof(Vertex, nx),        // Normal
                offsetof(Vertex, r),         // Color
                offsetof(Vertex, u),         // TexCoord
                offsetof(Vertex, boneIDs),   // BoneIDs (int)
                offsetof(Vertex, boneWeights) // BoneWeights
            };
        }
    };

    class VertexGroup {
        public:
            VertexGroup() = default;
    
            // Add a vertex to the buffer
            void AddVertex(const Vertex& vertex) {
                vertices.add(vertex);
            }

            void AddVertex(std::initializer_list<Vertex> vertexList) {
                vertices.add(vertexList);
            }
    
            // Get raw pointer to vertex data (for GPU upload)
            const Vertex* GetData() const { return vertices.GetData(); }

            // Get mutable pointer to vertex data (for bone weight assignment)
            Vertex* GetMutableData() { return const_cast<Vertex*>(vertices.GetData()); }

            // Get raw pointer to vertex data (for GPU upload)
            void* GetRawData() { return vertices.GetRawData(); }
    
            // Get the number of vertices
            size_t GetSize() const { return vertices.GetSize(); }
    
            // Get the total size of the buffer in bytes
            size_t GetSizeInBytes() const { return vertices.GetSize() * sizeof(Vertex); }
    
        private:
            Sleak::List<Vertex> vertices;
        };

    struct MeshData {
        VertexGroup vertices;
        IndexGroup indices;
    };

};

#endif

