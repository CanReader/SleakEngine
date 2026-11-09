#ifndef _RENDERCOMMANDQUEUE_H
#define _RENDERCOMMANDQUEUE_H

#include <Graphics/Common/Renderer.hpp>
#include <Memory/ObjectPtr.hpp>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "RenderCommands.hpp"

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

        bool HasCachedShadowDraws() const { return !cachedShadowDraws.empty(); }

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
            RenderCommandBase* command = nullptr;
            const RefPtr<BufferBase>* transformBuffer =
                nullptr;  // last-bound slot-0 buffer
            const RefPtr<BufferBase>* boneBuffer =
                nullptr;  // slot-3 bone UBO for skinned draws
            // Material bound before the draw, for alpha-tested casters
            ::Sleak::Material* material = nullptr;
        };

        private:
         /// Bump allocator owning one frame's commands until the slot is
         /// recycled.
         class CommandArena {
            public:
             CommandArena() = default;
             CommandArena(const CommandArena&) = delete;
             CommandArena& operator=(const CommandArena&) = delete;
             ~CommandArena() { Reset(); }

             /// Constructs a command in arena memory and appends it to the
             /// frame's list.
             template <typename T, typename... Args>
             T* Create(Args&&... args) {
                 void* memory = Allocate(sizeof(T), alignof(T));
                 T* command = new (memory) T(std::forward<Args>(args)...);
                 m_commands.push_back(command);
                 return command;
             }

             /// Destroys every command and rewinds the blocks for reuse.
             void Reset();

             const std::vector<RenderCommandBase*>& Commands() const {
                 return m_commands;
             }

            private:
             struct Block {
                 std::unique_ptr<std::byte[]> data;
                 size_t size = 0;
             };

             /// Returns aligned storage from the current block, adding blocks
             /// as needed.
             void* Allocate(size_t size, size_t alignment);

             static constexpr size_t BLOCK_SIZE = 64 * 1024;
             std::vector<Block> m_blocks;
             size_t m_blockIndex = 0;
             size_t m_offset = 0;
             std::vector<RenderCommandBase*> m_commands;
         };

         /// Moves submission to the next arena, destroying the commands it last
         /// held.
         void AdvanceArena();

         CommandArena& CurrentArena() { return m_arenas[m_currentArena]; }

         // A frame's commands stay alive while its shadow replay and GPU frames
         // in flight may use them
         static constexpr int RETIRE_FRAMES = 3;
         static constexpr int ARENA_COUNT = RETIRE_FRAMES + 2;
         static RenderCommandQueue* Instance;
         CommandArena m_arenas[ARENA_COUNT];
         int m_currentArena = 0;
         std::vector<ShadowDrawEntry> cachedShadowDraws;
         std::vector<RenderCommandBase*> m_pendingState;
         std::vector<RenderCommandBase*> m_forwardCmds;
        };
    }
}

#endif