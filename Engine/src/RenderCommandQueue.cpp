#include "../../include/private/Graphics/RenderCommandQueue.hpp"
#include "../../include/private/Graphics/BufferBase.hpp"
#include "../../include/private/Graphics/RenderContext.hpp"
#include "../../include/private/Graphics/RenderCommands.hpp"
#include <Runtime/Material.hpp>
#include <Memory/ObjectPtr.h>
#include <Logger.hpp>

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

            // Cache draw commands for shadow pass replay next frame,
            // pairing each with the last-bound slot-0 (transform) buffer.
            m_retiredShadowDraws[m_retireIndex].clear();
            m_retiredShadowDraws[m_retireIndex] = std::move(cachedShadowDraws);
            m_retireIndex = (m_retireIndex + 1) % RETIRE_FRAMES;
            cachedShadowDraws.clear();
            {
                RefPtr<BufferBase> lastSlot0Buffer;
                for (auto& cmd : commands) {
                    auto type = cmd->GetType();
                    if (type == CommandType::BindConstantBuffer) {
                        auto* bindCmd = static_cast<BindConstantBufferCommand*>(cmd.get());
                        if (bindCmd->GetSlot() == 0) {
                            lastSlot0Buffer = bindCmd->GetBuffer();
                        }
                    }
                    if ((type == CommandType::Draw || type == CommandType::DrawIndexed)
                        && lastSlot0Buffer) {
                        ShadowDrawEntry entry;
                        entry.command = cmd;
                        entry.transformBuffer = lastSlot0Buffer;
                        cachedShadowDraws.add(entry);
                    }
                }
            }

            // ---- Deferred rendering path ----
            // Split commands into:
            //   - Opaque group  → geometry pass (writes to GBuffer)
            //   - Forward group → forward transparent pass (after lighting)
            //
            // State commands (UpdateCB, BindCB, SetMode, SetFace) are accumulated
            // per-group and flushed together with the draw command they precede.
            // CustomCommands (skybox, debug lines) always go to the forward group.
            if (context->IsDeferredEnabled()) {

                // Per-group accumulator: state commands preceding the current draw.
                List<RefPtr<RenderCommandBase>> pendingState;
                List<RefPtr<RenderCommandBase>> forwardCmds;

                ::Sleak::Material* lastMaterial = nullptr;
                bool lastMaterialForward = false;

                auto isStateCmd = [](CommandType t) {
                    return t == CommandType::UpdateConstantBuffer
                        || t == CommandType::BindConstantBuffer
                        || t == CommandType::SetMode
                        || t == CommandType::SetFace;
                };

                for (auto& cmd : commands) {
                    auto type = cmd->GetType();

                    if (type == CommandType::BindMaterial) {
                        auto* bmc = static_cast<BindMaterialCommand*>(cmd.get());
                        lastMaterial = bmc->GetMaterial();
                        lastMaterialForward = lastMaterial && lastMaterial->IsForwardRendered();
                        // Always add material bind to pending state
                        pendingState.add(cmd);
                        continue;
                    }

                    if (type == CommandType::CustomCommand) {
                        // Skybox, debug lines — always rendered after the lighting pass.
                        // Flush any pending state to forward group first so state is correct.
                        for (auto& s : pendingState) forwardCmds.add(s);
                        pendingState.clear();
                        forwardCmds.add(cmd);
                        lastMaterial = nullptr;
                        lastMaterialForward = false;
                        continue;
                    }

                    if (isStateCmd(type)) {
                        pendingState.add(cmd);
                        continue;
                    }

                    if (type == CommandType::Draw || type == CommandType::DrawIndexed) {
                        if (lastMaterialForward) {
                            // Transparent/forward draw — defer to forward pass
                            for (auto& s : pendingState) forwardCmds.add(s);
                            forwardCmds.add(cmd);
                        } else {
                            // Opaque draw — execute now in geometry pass
                            for (auto& s : pendingState) s->Execute(context);
                            cmd->Execute(context);
                        }
                        pendingState.clear();
                        continue;
                    }

                    // Any other command type: execute immediately (opaque path)
                    cmd->Execute(context);
                }

                // Transition: geometry pass → lighting pass → forward transparent pass
                context->ExecuteDeferredLightingPass();

                context->BeginForwardTransparentPass();
                for (auto& cmd : forwardCmds)
                    cmd->Execute(context);
                context->EndForwardTransparentPass();

                commands.clear();
                return;
            }

            // ---- Forward rendering path (unchanged behavior) ----
            for (auto& cmd : commands)
                cmd->Execute(context);
            commands.clear();
        }

        void RenderCommandQueue::ExecuteShadowPass(RenderContext* context) {
            static int shadowPassDbg = 0;
            if (shadowPassDbg < 5) {
                SLEAK_INFO("ExecuteShadowPass: {} cached shadow draws", cachedShadowDraws.GetSize());
                ++shadowPassDbg;
            }
            for (size_t i = 0; i < cachedShadowDraws.GetSize(); ++i) {
                auto& entry = cachedShadowDraws[i];
                // Bind the transform buffer (slot 0) before drawing — shadow mode
                // in VulkanRenderer::BindConstantBuffer computes LightVP * World
                if (entry.transformBuffer) {
                    context->BindConstantBuffer(entry.transformBuffer, 0);
                }
                entry.command->ExecuteShadow(context);
            }
        }

        void RenderCommandQueue::SortCommands() {
            // Commands are submitted in the correct per-object order:
            // UpdateCB → BindCB → BindMaterial → Draw for each object,
            // with custom commands (skybox, debug lines) at their correct position.
            // Preserve submission order to maintain correct state bindings.
            // Future optimization: batch by material while preserving per-object groups.
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

