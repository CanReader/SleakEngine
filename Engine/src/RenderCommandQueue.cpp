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
            // TODO: Implement this method

            Queue<RefPtr<RenderCommandBase>> type3Queue;
            Queue<RefPtr<RenderCommandBase>> type4Queue;
            Queue<RefPtr<RenderCommandBase>> type1Queue;

            // Step 1: Categorize the commands
            while (!commands.isEmpty()) {
                auto command = commands.pop();
                if(command->GetType() == CommandType::SetFace)
                    std::cout << "sa";
                switch (command->GetType()) {
                    case CommandType::DrawIndexed: type1Queue.push(command); break;
                    case CommandType::UpdateConstantBuffer: type3Queue.push(command); break;
                    case CommandType::BindConstantBuffer: type4Queue.push(command); break;
                    default: break;
                }
            }

            // Step 2: Push back in the required 3-4-1 order
            while (!type3Queue.isEmpty() || !type4Queue.isEmpty() || !type1Queue.isEmpty()) {
                if (!type3Queue.isEmpty()) commands.push(type3Queue.pop());
                if (!type4Queue.isEmpty()) commands.push(type4Queue.pop());
                if (!type1Queue.isEmpty()) commands.push(type1Queue.pop());
            }
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

