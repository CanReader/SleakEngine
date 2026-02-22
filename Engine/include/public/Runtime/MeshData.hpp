#ifndef _MESHDATA_HPP_
#define _MESHDATA_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <Utility/Container/List.hpp>

namespace Sleak {

    using IndexType = uint32_t;
    using IndexGroup = List<IndexType>;

    struct Vertex {
        float px, py, pz;
        float nx, ny, nz;
        float tx, ty, tz, tw;
        float r, g, b, a;
        float u, v;
        int boneIDs[4] = {-1, -1, -1, -1};
        float boneWeights[4] = {0, 0, 0, 0};

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

        void SetPosition(float x, float y, float z) {
            px = x; py = y; pz = z;
        }

        void SetNormal(float x, float y, float z) {
            nx = x; ny = y; nz = z;
        }

        void SetColor(float r, float g, float b, float a) {
            this->r = r; this->g = g; this->b = b; this->a = a;
        }

        void SetTexCoord(float u, float v) {
            this->u = u; this->v = v;
        }

        static constexpr std::array<size_t, 6> GetAttributeSizes() {
            return { 3, 3, 4, 2, 4, 4 };
        }

        static constexpr size_t GetSize() {
            return sizeof(Vertex);
        }

        static constexpr std::array<size_t, 6> GetAttributeOffsets() {
            return {
                offsetof(Vertex, px),
                offsetof(Vertex, nx),
                offsetof(Vertex, r),
                offsetof(Vertex, u),
                offsetof(Vertex, boneIDs),
                offsetof(Vertex, boneWeights)
            };
        }
    };

    class VertexGroup {
    public:
        VertexGroup() = default;

        void AddVertex(const Vertex& vertex) {
            vertices.add(vertex);
        }

        void AddVertex(std::initializer_list<Vertex> vertexList) {
            vertices.add(vertexList);
        }

        const Vertex* GetData() const { return vertices.GetData(); }
        Vertex* GetMutableData() { return const_cast<Vertex*>(vertices.GetData()); }
        void* GetRawData() { return vertices.GetRawData(); }
        size_t GetSize() const { return vertices.GetSize(); }
        size_t GetSizeInBytes() const { return vertices.GetSize() * sizeof(Vertex); }

    private:
        Sleak::List<Vertex> vertices;
    };

    struct MeshData {
        VertexGroup vertices;
        IndexGroup indices;
    };

}

#endif
