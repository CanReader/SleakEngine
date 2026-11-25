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
    /// @ingroup rendering
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

    /// One indexed sub-draw of a mesh.
    /// @ingroup rendering
    struct MeshRange {
        uint32_t firstIndex = 0;  ///< first index read
        uint32_t indexCount = 0;  ///< indices drawn
        int32_t baseVertex = 0;   ///< added to every index
    };

    /// Passes a batched draw takes part in.
    /// @ingroup rendering
    enum class MeshPasses : uint8_t {
        Camera = 1u << 0,  ///< GBuffer, forward or transparent pass
        Shadow = 1u << 1,  ///< shadow pass (replayed next frame)
        All = Camera | Shadow,
    };

    /// GPU mesh handle pool for bulk static geometry. Draw through
    /// BeginBatch/Draw/EndBatch.
    ///
    /// Use this when you have far more geometry than you want GameObjects
    /// for: terrain chunks, voxel columns, instanced scatter, anything
    /// where per-object component overhead would dominate. CreateMesh()
    /// uploads vertex and index data and hands back a MeshHandle you store
    /// yourself; MeshBatch does not track it for you.
    ///
    /// The CreateMesh() overloads cover the two vertex paths. The
    /// VertexGroup overload uses the engine's built-in vertex layout. The
    /// raw-bytes overloads take a VertexFormatHandle from
    /// VertexFormatRegistry::Register, and the backend binds the pipeline
    /// that matches that handle. Meshes whose indices repeat one pattern,
    /// such as quads, can share one index buffer made by
    /// CreateSharedIndices() and upload only their vertices.
    ///
    /// Drawing is a three-step batch: BeginBatch() binds the material and
    /// an identity transform once, each Draw() issues one indexed draw
    /// call, and EndBatch() closes the batch. Passing `castsShadow = false`
    /// keeps distant geometry out of the shadow pass. DrawRanges() draws
    /// several index ranges of one mesh as one command, and its MeshPasses
    /// can keep that command to the camera passes or to the shadow pass.
    ///
    /// Every entry point touches renderer state, so call them from the
    /// thread that drives the frame, and call Shutdown() before the
    /// renderer is torn down.
    ///
    /// @code{.cpp}
    /// // Once at startup, describe and register the vertex layout
    /// Sleak::VertexLayoutDesc desc;
    /// desc.stride = sizeof(MyVertex);
    /// desc.attributes = {
    ///     {0, Sleak::VertexAttribFormat::Float3, offsetof(MyVertex, pos)},
    ///     {1, Sleak::VertexAttribFormat::Float3, offsetof(MyVertex, normal)},
    /// };
    /// desc.shaderStem = "my_forward";
    /// Sleak::VertexFormatHandle fmt =
    ///     Sleak::VertexFormatRegistry::Register(desc);
    ///
    /// // Per chunk, build GPU buffers
    /// Sleak::MeshHandle mesh = Sleak::MeshBatch::CreateMesh(
    ///     fmt, vertices.data(), vertices.size() * sizeof(MyVertex),
    ///     indices.data(), indices.size());
    ///
    /// // Every frame, submit the visible ones
    /// Sleak::MeshBatch::BeginBatch(material);
    /// for (const auto& chunk : visibleChunks) {
    ///     if (chunk.mesh.IsValid()) Sleak::MeshBatch::Draw(chunk.mesh);
    /// }
    /// Sleak::MeshBatch::EndBatch();
    /// @endcode
    ///
    /// @see MeshHandle, VertexLayoutDesc, VertexFormatRegistry, Material,
    ///      MeshComponent
    /// @ingroup rendering
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

        /// Uploads an index buffer for many meshes to share. The handle has
        /// no vertex buffer and is never drawn; the buffer lives while it or
        /// any mesh created from it does.
        static MeshHandle CreateSharedIndices(const uint32_t* indices,
                                              size_t indexCount);

        /// Uploads raw vertices of a registered format that draw the first
        /// indexCount indices of shared; invalid if shared holds fewer.
        static MeshHandle CreateMesh(VertexFormatHandle format,
                                     const void* vertexData, size_t vertexBytes,
                                     const MeshHandle& shared,
                                     size_t indexCount);

        /// Begin a batch: binds the material and an identity-transform
        /// constant buffer once.  All subsequent Draw() calls share them.
        static void BeginBatch(Material* material);

        /// Submit one indexed draw call (vertex + index buffer bind + DrawIndexed).
        /// castsShadow=false makes the draw skip the shadow pass (distant geometry).
        static void Draw(const MeshHandle& mesh, bool castsShadow = true);

        /// Submits several ranges of one mesh as one command: buffers bind
        /// once. The ranges are copied, so the array can be reused at once.
        static void DrawRanges(const MeshHandle& mesh, const MeshRange* ranges,
                               uint32_t count,
                               MeshPasses passes = MeshPasses::All);

        /// End the batch (currently a no-op, reserved for future use).
        static void EndBatch();

        /// Release static resources before renderer cleanup.
        static void Shutdown();
    };

}

#endif
