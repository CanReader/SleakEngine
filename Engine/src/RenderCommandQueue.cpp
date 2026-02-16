#include "../../include/private/Graphics/RenderCommandQueue.hpp"
#include "../../include/private/Graphics/BufferBase.hpp"
#include "../../include/private/Graphics/RenderContext.hpp"
#include <Memory/ObjectPtr.h>

namespace Sleak {
    namespace RenderEngine {
        RenderCommandQueue* RenderCommandQueue::Instance = nullptr; 

        
        void RenderCommandQueue::SubmitDraw( RefPtr<BufferBase> vertexBuffer,
                                            List<RefPtr<BufferBase>> constantBuffers, 
                                            uint32_t vertexCount, 
                                            uint32_t startVertexLocation) {
            auto command = RefPtr<RenderCommandBase>(new DrawCommand(vertexBuffer,constantBuffers,vertexCount, startVertexLocation));
            commands.push(std::move(command));
        }

        void RenderCommandQueue::SubmitDrawIndexed(
            RefPtr<BufferBase> vertexBuffer, RefPtr<BufferBase> indexBuffer,
            List<RefPtr<BufferBase>> constantBuffers, uint32_t indexCount,
            uint32_t startIndexLocation, int32_t baseVertexLocation) {
            auto command = RefPtr<RenderCommandBase>(new DrawIndexedCommand(
                vertexBuffer, indexBuffer, constantBuffers, indexCount,
                startIndexLocation, baseVertexLocation));
            commands.push(std::move(command));
        }

        void RenderCommandQueue::SubmitBindConstantBuffer(RefPtr<BufferBase> buffer, uint8_t slot) {
            auto command = RefPtr<RenderCommandBase>(new BindConstantBufferCommand(buffer, slot));
            commands.push(std::move(command));
        }

        void RenderCommandQueue::SubmitUpdateConstantBuffer(RefPtr<BufferBase> buffer, void* Data, uint16_t Size) {
            auto command = RefPtr<RenderCommandBase>(new UpdateConstantBufferCommand(buffer, Data, Size));
            commands.push(std::move(command));
        }

        void RenderCommandQueue::SubmitBindMaterial(::Sleak::Material* material) {
            auto command = RefPtr<RenderCommandBase>(new BindMaterialCommand(material));
            commands.push(std::move(command));
        }

        void RenderCommandQueue::SubmitSetRenderMode(RenderMode mode) {
            auto command = RefPtr<RenderCommandBase>(new SetRenderModeCommand(mode));
            commands.push(command);
        }

        void RenderCommandQueue::SubmitSetRenderFace(RenderFace face) {
            auto command = RefPtr<RenderCommandBase>(new SetRenderFaceCommand(face));
            commands.push(command);
        }

        void RenderCommandQueue::SubmitCustomCommand(CustomCommand::ExecuteFunction function) {
            auto command =
                RefPtr<RenderCommandBase>(new CustomCommand(function)); 
            commands.push(std::move(command));

        }

        void RenderCommandQueue::ExecuteCommands(RenderContext* context) {
            
            SortCommands();
            OptimizeBatching();

            while (!commands.isEmpty())  
                commands.pop()->Execute(context);
        }

        void RenderCommandQueue::SortCommands() {
            // Sorting is also important for minimizing state changes.

            Queue<RefPtr<RenderCommandBase>> customQueue;
            Queue<RefPtr<RenderCommandBase>> updateCBQueue;    // UpdateConstantBuffer
            Queue<RefPtr<RenderCommandBase>> bindCBQueue;      // BindConstantBuffer
            Queue<RefPtr<RenderCommandBase>> materialQueue;    // BindMaterial
            Queue<RefPtr<RenderCommandBase>> drawQueue;        // DrawIndexed + Draw

            // Step 1: Categorize the commands
            while (!commands.isEmpty()) {
                auto command = commands.pop();
                switch (command->GetType()) {
                    case CommandType::Draw:
                    case CommandType::DrawIndexed:        drawQueue.push(command); break;
                    case CommandType::UpdateConstantBuffer: updateCBQueue.push(command); break;
                    case CommandType::BindConstantBuffer: bindCBQueue.push(command); break;
                    case CommandType::BindMaterial:       materialQueue.push(command); break;
                    case CommandType::CustomCommand:      customQueue.push(command); break;
                    // State commands: keep in draw queue to preserve order
                    case CommandType::SetMode:
                    case CommandType::SetFace:            drawQueue.push(command); break;
                    default: break;
                }
            }

            // Step 2: Interleave per-object: UpdateCB → BindCB → BindMaterial → Draw
            // Then custom commands last (e.g. skybox)
            while (!updateCBQueue.isEmpty() || !bindCBQueue.isEmpty() ||
                   !materialQueue.isEmpty() || !drawQueue.isEmpty()) {
                if (!updateCBQueue.isEmpty()) commands.push(updateCBQueue.pop());
                if (!bindCBQueue.isEmpty())   commands.push(bindCBQueue.pop());
                if (!materialQueue.isEmpty()) commands.push(materialQueue.pop());
                if (!drawQueue.isEmpty())     commands.push(drawQueue.pop());
            }
            while (!customQueue.isEmpty()) commands.push(customQueue.pop());
        }

        void RenderCommandQueue::OptimizeBatching() {
            // Batching is a technique to call render commands as single pass instead pf multiple draw calls. This significantly improves rendering efficiency
            // TODO: Implement this method
        }

        void RenderCommandQueue::Clear() {
            while (!commands.isEmpty()) 
                commands.pop();
        }
    }
}

