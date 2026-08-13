#ifndef _MESHBATCH_HPP_
#define _MESHBATCH_HPP_

#include <Core/OSDef.hpp>
#include <Runtime/MeshData.hpp>
#include <Memory/RefPtr.hpp>
#include <cstdint>

namespace Sleak {

    class Material;

    namespace RenderEngine {
        class BufferBase;
    }

    /// Lightweight handle for GPU mesh buffers created outside the
    /// GameObject/Component system.  Allows bulk draw submission with
    /// minimal per-object overhead (no TransformComponent recalcs, no
    /// per-draw material binds).
    struct ENGINE_API MeshHandle {
        RefPtr<RenderEngine::BufferBase> vertexBuffer;
        RefPtr<RenderEngine::BufferBase> indexBuffer;
        uint32_t indexCount = 0;
        bool isVoxelFormat = false;

        /// Special members defined out-of-line in MeshBatch.cpp so that
        /// RefPtr<BufferBase>::release() is instantiated where BufferBase
        /// is a complete type. With only a forward declaration, `delete`
        /// inside release() skips ~BufferBase() entirely and leaks the
        /// underlying VkBuffer/VkDeviceMemory forever.
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

        /// Create GPU vertex+index buffers from compact voxel mesh data.
        static MeshHandle CreateVoxelMesh(VoxelVertexGroup& vertices,
                                          IndexGroup& indices);

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
