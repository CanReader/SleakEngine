#ifndef _RENDERCOMMANDQUEUE_H
#define _RENDERCOMMANDQUEUE_H

#include <Graphics/Renderer.hpp>
#include "RenderCommands.hpp"
#include <Utility/Container/Queue.hpp>
#include <Memory/ObjectPtr.h>

namespace Sleak {
    namespace RenderEngine {
        class RenderCommandQueue {
        public:
        void SubmitDrawIndexed(
            RefPtr<BufferBase> vertexBuffer,
            RefPtr<BufferBase> indexBuffer,
            List<RefPtr<BufferBase>> constantBuffers,
            uint32_t indexCount,
            uint32_t startIndexLocation = 0,
            int32_t baseVertexLocation = 0
        );

        void SubmitDraw(
            RefPtr<BufferBase> vertexBuffer,
            List<RefPtr<BufferBase>> constantBuffers,
            uint32_t vertexCount,
            uint32_t startVertexLocation = 0
        );

        void SubmitBindConstantBuffer(
            RefPtr<BufferBase> buffer,
            uint8_t slot
        );

        void SubmitUpdateConstantBuffer(
            RefPtr<BufferBase> buffer,
            void* Data,
            uint16_t Size
        );

        void SubmitBindMaterial(::Sleak::Material* material);

        void SubmitSetRenderMode(RenderMode mode);

        void SubmitSetRenderFace(RenderFace face);

        void SubmitCustomCommand(CustomCommand::ExecuteFunction function);

        void ExecuteCommands(RenderContext* context);

        void ExecuteShadowPass(RenderContext* context);

        void Clear();

        void SortCommands();
        void OptimizeBatching();
        
        inline static RenderCommandQueue* GetInstance() 
        {
            Instance = Instance ? Instance : new RenderCommandQueue();
            return Instance;
        }

        struct ShadowDrawEntry {
            RefPtr<RenderCommandBase> command;
            RefPtr<BufferBase> transformBuffer;  // last-bound slot-0 buffer
        };

        private:
            static RenderCommandQueue* Instance;
            Queue<RefPtr<RenderCommandBase>> commands;
            List<ShadowDrawEntry> cachedShadowDraws;
        };
    }
}

#endif