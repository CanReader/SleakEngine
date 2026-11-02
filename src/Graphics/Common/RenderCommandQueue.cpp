#include "../../include/private/Graphics/Common/RenderCommandQueue.hpp"
#include "../../include/private/Graphics/Common/BufferBase.hpp"
#include "../../include/private/Graphics/Common/RenderContext.hpp"
#include "../../include/private/Graphics/Common/RenderCommands.hpp"
#include <Runtime/Material.hpp>
#include <Memory/ObjectPtr.hpp>
#include <Core/Logger.hpp>
#include <algorithm>

namespace Sleak {
    namespace RenderEngine {
        RenderCommandQueue* RenderCommandQueue::Instance = nullptr; 

        
        void RenderCommandQueue::SubmitDraw( RefPtr<BufferBase> vertexBuffer,
                                            List<RefPtr<BufferBase>> constantBuffers, 
                                            uint32_t vertexCount, 
                                            uint32_t startVertexLocation) {
            CurrentArena().Create<DrawCommand>(
                std::move(vertexBuffer), std::move(constantBuffers),
                vertexCount, startVertexLocation);
        }

        void RenderCommandQueue::SubmitDrawIndexed(
            RefPtr<BufferBase> vertexBuffer, RefPtr<BufferBase> indexBuffer,
            List<RefPtr<BufferBase>> constantBuffers, uint32_t indexCount,
            uint32_t startIndexLocation, int32_t baseVertexLocation,
            bool castsShadow) {
            auto* command = CurrentArena().Create<DrawIndexedCommand>(
                std::move(vertexBuffer), std::move(indexBuffer),
                std::move(constantBuffers), indexCount, startIndexLocation,
                baseVertexLocation);
            command->SetCastsShadow(castsShadow);
        }

        void RenderCommandQueue::SubmitBindConstantBuffer(RefPtr<BufferBase> buffer, uint8_t slot) {
            CurrentArena().Create<BindConstantBufferCommand>(std::move(buffer), slot);
        }

        void RenderCommandQueue::SubmitUpdateConstantBuffer(RefPtr<BufferBase> buffer, void* Data, uint16_t Size) {
            CurrentArena().Create<UpdateConstantBufferCommand>(std::move(buffer), Data, Size);
        }

        void RenderCommandQueue::SubmitBindMaterial(::Sleak::Material* material) {
            CurrentArena().Create<BindMaterialCommand>(material);
        }

        void RenderCommandQueue::SubmitSetRenderMode(RenderMode mode) {
            CurrentArena().Create<SetRenderModeCommand>(mode);
        }

        void RenderCommandQueue::SubmitSetRenderFace(RenderFace face) {
            CurrentArena().Create<SetRenderFaceCommand>(face);
        }

        void RenderCommandQueue::SubmitCustomCommand(CustomCommand::ExecuteFunction function) {
            CurrentArena().Create<CustomCommand>(std::move(function));
        }

        void RenderCommandQueue::ExecuteCommands(RenderContext* context) {

            SortCommands();
            OptimizeBatching();

            const auto& commands = CurrentArena().Commands();

            // Cache draw commands for shadow pass replay next frame,
            // pairing each with the last-bound slot-0 (transform) buffer.
            cachedShadowDraws.clear();
            {
                const RefPtr<BufferBase>* lastSlot0Buffer = nullptr;
                const RefPtr<BufferBase>* lastSlot3Buffer = nullptr;
                for (RenderCommandBase* cmd : commands) {
                    auto type = cmd->GetType();
                    if (type == CommandType::BindConstantBuffer) {
                        auto* bindCmd = static_cast<BindConstantBufferCommand*>(cmd);
                        if (bindCmd->GetSlot() == 0) {
                            lastSlot0Buffer = &bindCmd->GetBuffer();
                        } else if (bindCmd->GetSlot() == 3) {
                            lastSlot3Buffer = &bindCmd->GetBuffer();
                        }
                    }
                    if ((type == CommandType::Draw || type == CommandType::DrawIndexed)
                        && lastSlot0Buffer && *lastSlot0Buffer && cmd->CastsShadow()) {
                        ShadowDrawEntry entry;
                        entry.command = cmd;
                        entry.transformBuffer = lastSlot0Buffer;
                        // Pair bone buffer only with skinned draws so non-skinned
                        // casters don't accidentally inherit a stale bone UBO.
                        if (cmd->IsSkinned())
                            entry.boneBuffer = lastSlot3Buffer;
                        cachedShadowDraws.push_back(entry);
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
                auto& pendingState = m_pendingState;
                auto& forwardCmds = m_forwardCmds;
                pendingState.clear();
                forwardCmds.clear();

                ::Sleak::Material* lastMaterial = nullptr;
                bool lastMaterialForward = false;

                auto isStateCmd = [](CommandType t) {
                    return t == CommandType::UpdateConstantBuffer
                        || t == CommandType::BindConstantBuffer
                        || t == CommandType::SetMode
                        || t == CommandType::SetFace;
                };

                for (RenderCommandBase* cmd : commands) {
                    auto type = cmd->GetType();

                    if (type == CommandType::BindMaterial) {
                        auto* bmc = static_cast<BindMaterialCommand*>(cmd);
                        lastMaterial = bmc->GetMaterial();
                        lastMaterialForward = lastMaterial && lastMaterial->IsForwardRendered();
                        // Always add material bind to pending state
                        pendingState.push_back(cmd);
                        continue;
                    }

                    if (type == CommandType::CustomCommand) {
                        // Skybox, debug lines — always rendered after the lighting pass.
                        // Flush any pending state to forward group first so state is correct.
                        forwardCmds.insert(forwardCmds.end(), pendingState.begin(), pendingState.end());
                        pendingState.clear();
                        forwardCmds.push_back(cmd);
                        lastMaterial = nullptr;
                        lastMaterialForward = false;
                        continue;
                    }

                    if (isStateCmd(type)) {
                        pendingState.push_back(cmd);
                        continue;
                    }

                    if (type == CommandType::Draw || type == CommandType::DrawIndexed) {
                        // Skinned meshes have no GBuffer pipeline variant — the
                        // skinned pipeline is render-pass-compatible only with
                        // the forward pass, so route them there even when opaque.
                        const bool isSkinned = cmd->IsSkinned();
                        if (lastMaterialForward || isSkinned) {
                            // Transparent/forward/skinned draw — defer to forward pass
                            forwardCmds.insert(forwardCmds.end(), pendingState.begin(), pendingState.end());
                            forwardCmds.push_back(cmd);
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
                for (RenderCommandBase* cmd : forwardCmds)
                    cmd->Execute(context);
                context->EndForwardTransparentPass();

                AdvanceArena();
                return;
            }

            // ---- Forward rendering path (unchanged behavior) ----
            for (RenderCommandBase* cmd : commands)
                cmd->Execute(context);
            AdvanceArena();
        }

        void RenderCommandQueue::ExecuteShadowPass(RenderContext* context) {
            for (const ShadowDrawEntry& entry : cachedShadowDraws) {
                // Bind the transform buffer (slot 0) before drawing — shadow mode
                // in VulkanRenderer::BindConstantBuffer computes LightVP * World
                if (entry.transformBuffer && *entry.transformBuffer) {
                    context->BindConstantBuffer(*entry.transformBuffer, 0);
                }
                // Skinned casters need their bone matrices bound so the shadow
                // depth vert can transform verts into the posed skeleton space.
                if (entry.boneBuffer && *entry.boneBuffer) {
                    context->BindBoneBuffer(*entry.boneBuffer);
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
            CurrentArena().Reset();
        }

        void RenderCommandQueue::ClearAll() {
            cachedShadowDraws.clear();
            m_pendingState.clear();
            m_forwardCmds.clear();
            for (CommandArena& arena : m_arenas)
                arena.Reset();
            m_currentArena = 0;
        }

        void RenderCommandQueue::AdvanceArena() {
            m_pendingState.clear();
            m_forwardCmds.clear();
            m_currentArena = (m_currentArena + 1) % ARENA_COUNT;
            CurrentArena().Reset();
        }

        void* RenderCommandQueue::CommandArena::Allocate(size_t size,
                                                         size_t alignment) {
            for (;;) {
                if (m_blockIndex < m_blocks.size()) {
                    Block& block = m_blocks[m_blockIndex];
                    const size_t start =
                        (m_offset + alignment - 1) & ~(alignment - 1);
                    if (start + size <= block.size) {
                        m_offset = start + size;
                        return block.data.get() + start;
                    }
                    ++m_blockIndex;
                    m_offset = 0;
                    continue;
                }
                const size_t blockSize = std::max(BLOCK_SIZE, size + alignment);
                m_blocks.push_back(
                    {std::make_unique<std::byte[]>(blockSize), blockSize});
            }
        }

        void RenderCommandQueue::CommandArena::Reset() {
            for (RenderCommandBase* command : m_commands)
                command->~RenderCommandBase();
            m_commands.clear();
            m_blockIndex = 0;
            m_offset = 0;
        }

        void RenderCommandQueue::Shutdown() {
            if (Instance) {
                Instance->ClearAll();
                delete Instance;
                Instance = nullptr;
            }
        }
    }
}

