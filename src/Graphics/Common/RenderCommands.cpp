#include "../../include/private/Graphics/Common/RenderContext.hpp"
#include "../../include/private/Graphics/Common/RenderCommands.hpp"
#include "../../include/private/Graphics/Common/BufferBase.hpp"
#include "../../include/private/Graphics/Common/Renderer.hpp"
#include "../../include/private/Graphics/Common/ConstantBuffer.hpp"
#include "../../include/private/Graphics/Common/Shader.hpp"
#include "../../include/public/Runtime/Texture.hpp"
#include <Runtime/Material.hpp>

namespace Sleak {
namespace RenderEngine {

DrawCommand::DrawCommand(RefPtr<BufferBase> buffer,
                         List<RefPtr<BufferBase>> constantBuffers,
                         uint32_t vertexCount, uint32_t vertexLocation)
    : m_vertexBuffer(buffer),
      m_constantBuffers(constantBuffers),
      m_vertexCount(vertexCount),
      m_startVertexLocation(vertexLocation) {}

void DrawCommand::Execute(RenderContext* context) {
    context->BindVertexBuffer(m_vertexBuffer, m_startVertexLocation);

    for (const auto& b : m_constantBuffers)
        if (b) context->BindConstantBuffer(b, b->GetSlot());

    context->Draw(m_vertexCount);
}

void DrawCommand::ExecuteShadow(RenderContext* context) {
    context->BindVertexBuffer(m_vertexBuffer, m_startVertexLocation);
    for (const auto& b : m_constantBuffers) {
        if (b && b->GetSlot() == 0) {
            context->BindConstantBuffer(b, 0);
            break;
        }
    }
    context->Draw(m_vertexCount);
}

bool DrawCommand::IsSkinned() const {
    for (const auto& b : m_constantBuffers) {
        if (b && b->GetSlot() == 3) return true;
    }
    return false;
}

DrawIndexedCommand::DrawIndexedCommand(RefPtr<BufferBase> vertexBuffer,
                                       RefPtr<BufferBase> indexBuffer,
                                       List<RefPtr<BufferBase>> constantBuffers,
                                       uint32_t indexCount,
                                       uint32_t startIndexLocation,
                                       int32_t baseVertexLocation)
    : m_vertexBuffer(vertexBuffer),
      m_indexBuffer(indexBuffer),
      m_constantBuffers(constantBuffers),
      m_indexCount(indexCount),
      m_startIndexLocation(startIndexLocation),
      m_baseVertexLocation(baseVertexLocation) {}

/// Binds buffers, routes bone data through the skinned pipeline when present, then draws.
void DrawIndexedCommand::Execute(RenderContext* context) {
    context->BindVertexBuffer(m_vertexBuffer, m_startIndexLocation);
    context->BindIndexBuffer(m_indexBuffer, m_startIndexLocation);

    // Detect if this is a skinned mesh (has bone buffer at slot 3)
    bool hasBones = false;
    for (const auto& b : m_constantBuffers) {
        if (b && b->GetSlot() == 3) { hasBones = true; break; }
    }

    // Switch to skinned pipeline before binding any constants
    // (Vulkan pipeline change invalidates push constants)
    if (hasBones) context->BeginSkinnedPass();

    for (const auto& b : m_constantBuffers) {
        if (!b) continue;
        if (b->GetSlot() == 3) {
            context->BindBoneBuffer(b);
        } else {
            context->BindConstantBuffer(b, b->GetSlot());
        }
    }

    context->DrawIndexed(m_indexCount);

    // Switch back to default pipeline for subsequent non-skinned draws
    if (hasBones) context->EndSkinnedPass();
}

void DrawIndexedCommand::ExecuteShadow(RenderContext* context) {
    context->BindVertexBuffer(m_vertexBuffer, m_startIndexLocation);
    context->BindIndexBuffer(m_indexBuffer, m_startIndexLocation);
    for (const auto& b : m_constantBuffers) {
        if (b && b->GetSlot() == 0) {
            context->BindConstantBuffer(b, 0);
            break;
        }
    }
    context->DrawIndexed(m_indexCount);
}

bool DrawIndexedCommand::IsSkinned() const {
    for (const auto& b : m_constantBuffers) {
        if (b && b->GetSlot() == 3) return true;
    }
    return false;
}

/// Copies the payload into inline storage, falling back to a heap allocation past INLINE_CAPACITY.
UpdateConstantBufferCommand::UpdateConstantBufferCommand(RefPtr<BufferBase> buffer,
    void* Data,
    uint16_t Size) :
      constantBuffer(buffer),
      Size(Size) {
    if (Size <= INLINE_CAPACITY) {
        // Fast path: no heap allocation for small buffers
        memcpy(m_inlineData, Data, Size);
    } else {
        // Fallback for large buffers
        m_heapData = malloc(Size);
        if (m_heapData) {
            memcpy(m_heapData, Data, Size);
        } else {
            SLEAK_ERROR("Failed to allocate memory at UpdateConstantBufferCommand!");
        }
    }
}

void UpdateConstantBufferCommand::Execute(RenderContext* context) {
    void* data = m_heapData ? m_heapData : static_cast<void*>(m_inlineData);
    constantBuffer->Update(data, Size);
}

BindConstantBufferCommand::BindConstantBufferCommand(RefPtr<BufferBase> buffer, int slot) : 
      constantBuffer(buffer), slot(slot) {}

void BindConstantBufferCommand::Execute(RenderContext* context) {
    context->BindConstantBuffer(constantBuffer, slot);
}

SetRenderModeCommand::SetRenderModeCommand(RenderMode mode) : mode(mode) {}

void SetRenderModeCommand::Execute(RenderContext* context) {
    context->SetRenderMode(mode);
}

SetRenderFaceCommand::SetRenderFaceCommand(RenderFace face) : face(face) {}

void SetRenderFaceCommand::Execute(RenderContext* context) {
    context->SetRenderFace(face);
}

BindMaterialCommand::BindMaterialCommand(::Sleak::Material* material)
    : m_material(material) {}

/// Binds the material either through the deferred PBR path or the plain forward path, per active pass.
void BindMaterialCommand::Execute(RenderContext* context) {
    if (m_material) {
        if (context->IsInGeometryPass()) {
            // PBR deferred geometry pass: bind pipeline + all 6 PBR textures +
            // material UBO as a single Vulkan descriptor set update.
            context->BindPBRMaterial(m_material);
        } else {
            m_material->Bind();
            // Bind diffuse texture through RenderContext (needed for Vulkan
            // descriptor set switching — OpenGL already binds via Texture::Bind())
            auto* diffuse = m_material->GetDiffuseTexture();
            if (diffuse) context->BindTextureRaw(diffuse, 0);
        }
    }
}

CustomCommand::CustomCommand(ExecuteFunction function)
    : m_executeFunction(function) {}

void CustomCommand::Execute(RenderContext* context) { 
    if (m_executeFunction)
        m_executeFunction(context);
}
    
}

}
