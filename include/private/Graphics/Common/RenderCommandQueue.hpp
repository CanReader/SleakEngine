#ifndef _RENDERCOMMANDQUEUE_H
#define _RENDERCOMMANDQUEUE_H

#include <Graphics/Common/Renderer.hpp>
#include "RenderCommands.hpp"
#include <Utility/Container/Queue.hpp>
#include <Memory/ObjectPtr.hpp>

namespace Sleak {
    namespace RenderEngine {
        /// Frame-scoped queue of recorded draw/state commands, replayed into a RenderContext at flush time.
        class RenderCommandQueue {
        public:
        /// Queues an indexed draw with its vertex/index buffers and constant buffers.
        void SubmitDrawIndexed(
            RefPtr<BufferBase> vertexBuffer,
            RefPtr<BufferBase> indexBuffer,
            List<RefPtr<BufferBase>> constantBuffers,
            uint32_t indexCount,
            uint32_t startIndexLocation = 0,
            int32_t baseVertexLocation = 0,
            bool castsShadow = true
        );

        /// Queues a non-indexed draw with its vertex buffer and constant buffers.
        void SubmitDraw(
            RefPtr<BufferBase> vertexBuffer,
            List<RefPtr<BufferBase>> constantBuffers,
            uint32_t vertexCount,
            uint32_t startVertexLocation = 0
        );

        /// Queues a constant buffer bind at the given slot.
        void SubmitBindConstantBuffer(
            RefPtr<BufferBase> buffer,
            uint8_t slot
        );

        /// Queues a constant buffer write with data captured at submit time.
        void SubmitUpdateConstantBuffer(
            RefPtr<BufferBase> buffer,
            void* Data,
            uint16_t Size
        );

        /// Queues a material bind, switching shader/texture state for subsequent draws.
        void SubmitBindMaterial(::Sleak::Material* material);

        void SubmitSetRenderMode(RenderMode mode);

        void SubmitSetRenderFace(RenderFace face);

        /// Queues an arbitrary callback to run inline with other render commands.
        void SubmitCustomCommand(CustomCommand::ExecuteFunction function);

        // Execute all queued commands.
        // In deferred mode: runs opaque draws in the geometry pass, then triggers the
        // lighting pass, then runs transparent/custom draws in the forward pass.
        // In forward mode: runs all commands in order (legacy behavior).
        void ExecuteCommands(RenderContext* context);

        void ExecuteShadowPass(RenderContext* context);

        bool HasCachedShadowDraws() const { return cachedShadowDraws.GetSize() > 0; }

        /// Drops queued commands for the current frame, keeping the cached shadow draw list.
        void Clear();

        /// Drops queued commands and the cached shadow draw list.
        void ClearAll();
        static void Shutdown();

        void SortCommands();
        void OptimizeBatching();
        
        /// Lazily creates and returns the process-wide singleton instance.
        inline static RenderCommandQueue* GetInstance() 
        {
            Instance = Instance ? Instance : new RenderCommandQueue();
            return Instance;
        }

        /// A draw command paired with the transform/bone buffers it needs to replay in the shadow pass.
        struct ShadowDrawEntry {
            RefPtr<RenderCommandBase> command;
            RefPtr<BufferBase> transformBuffer;  // last-bound slot-0 buffer
            RefPtr<BufferBase> boneBuffer;       // slot-3 bone UBO for skinned draws
        };

        private:
            static constexpr int RETIRE_FRAMES = 3;
            static RenderCommandQueue* Instance;
            Queue<RefPtr<RenderCommandBase>> commands;
            List<ShadowDrawEntry> cachedShadowDraws;
            List<ShadowDrawEntry> m_retiredShadowDraws[RETIRE_FRAMES];
            int m_retireIndex = 0;
        };
    }
}

#endif