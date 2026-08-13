#ifndef _MESHBATCH_HPP_
#define _MESHBATCH_HPP_

#include <Core/OSDef.hpp>
#include <Runtime/MeshData.hpp>
#include <Runtime/VertexLayout.hpp>
#include <Memory/RefPtr.hpp>
#include <cstddef>
#include <cstdint>

namespace Sleak {

    class Material;

    namespace RenderEngine {
        class BufferBase;
    }

    /// Lightweight handle for GPU mesh buffers created outside the
    /// GameObject/Component system, for bulk draw submission with
    /// minimal per-object overhead (no per-draw component work).
    struct ENGINE_API MeshHandle {
        RefPtr<RenderEngine::BufferBase> vertexBuffer;
        RefPtr<RenderEngine::BufferBase> indexBuffer;
        uint32_t indexCount = 0;
        /// Registered custom vertex layout of this mesh; 0 means the engine default Vertex.
        uint32_t vertexFormat = 0;

        /// Special members defined out-of-line in MeshBatch.cpp so
        /// RefPtr<BufferBase>::release() sees a complete BufferBase type;
        /// a forward declaration alone would skip ~BufferBase() and leak the GPU buffer.
        MeshHandle();
        ~MeshHandle();
        MeshHandle(const MeshHandle&);
        MeshHandle(MeshHandle&&) noexcept;
        MeshHandle& operator=(const MeshHandle&);
        MeshHandle& operator=(MeshHandle&&) noexcept;

        bool IsValid() const {
            return vertexBuffer.IsValid() && indexBuffer.IsValid() && indexCount > 0;
        }
    };

    /// GPU mesh handle pool for bulk static geometry. Draw through BeginBatch/Draw/EndBatch.
    class ENGINE_API MeshBatch {
    public:
        /// Create GPU vertex+index buffers from CPU mesh data.
        static MeshHandle CreateMesh(VertexGroup& vertices, IndexGroup& indices);

        /// Create GPU buffers from raw vertex bytes laid out per a registered
        /// custom vertex format. The handle keys the pipeline the backend binds.
        static MeshHandle CreateMesh(VertexFormatHandle format,
                                     const void* vertexData,
                                     size_t vertexBytes,
                                     const uint32_t* indices,
                                     size_t indexCount);

        /// Begin a batch: binds the material and an identity-transform
        /// constant buffer once.  All subsequent Draw() calls share them.
        static void BeginBatch(Material* material);

        /// Submit one indexed draw call (vertex + index buffer bind + DrawIndexed).
        /// castsShadow=false makes the draw skip the shadow pass (distant geometry).
        static void Draw(const MeshHandle& mesh, bool castsShadow = true);

        /// End the batch (currently a no-op, reserved for future use).
        static void EndBatch();

        /// Release static resources before renderer cleanup.
        static void Shutdown();
    };

}

#endif
