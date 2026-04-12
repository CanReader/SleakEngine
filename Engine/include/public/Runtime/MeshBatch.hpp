#ifndef _MESHBATCH_HPP_
#define _MESHBATCH_HPP_

#include <Core/OSDef.hpp>
#include <Runtime/MeshData.hpp>
#include <Memory/RefPtr.h>
#include <cstdint>

namespace Sleak {

    class Material;

    namespace RenderEngine {
        class BufferBase;
    }

    // Lightweight handle for GPU mesh buffers created outside the
    // GameObject/Component system.  Allows bulk draw submission with
    // minimal per-object overhead (no TransformComponent recalcs, no
    // per-draw material binds).
    struct MeshHandle {
        RefPtr<RenderEngine::BufferBase> vertexBuffer;
        RefPtr<RenderEngine::BufferBase> indexBuffer;
        uint32_t indexCount = 0;

        bool IsValid() const {
            return vertexBuffer.IsValid() && indexBuffer.IsValid() && indexCount > 0;
        }
    };

    class ENGINE_API MeshBatch {
    public:
        // Create GPU vertex+index buffers from CPU mesh data.
        static MeshHandle CreateMesh(VertexGroup& vertices, IndexGroup& indices);

        // Begin a batch: binds the material and an identity-transform
        // constant buffer once.  All subsequent Draw() calls share them.
        static void BeginBatch(Material* material);

        // Submit one indexed draw call (vertex + index buffer bind + DrawIndexed).
        static void Draw(const MeshHandle& mesh);

        // End the batch (currently a no-op, reserved for future use).
        static void EndBatch();

        // Release static resources before renderer cleanup.
        static void Shutdown();
    };

}

#endif
