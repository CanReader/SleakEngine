#include "../../include/private/Graphics/RenderContext.hpp"
#include "../../include/private/Graphics/RenderCommands.hpp"
#include "../../include/private/Graphics/BufferBase.hpp"
#include "../../include/private/Graphics/Renderer.hpp"
#include "../../include/private/Graphics/ConstantBuffer.hpp"
#include "../../include/private/Graphics/Shader.hpp"
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

void BindMaterialCommand::Execute(RenderContext* context) {
    if (m_material) {
        if (context->IsInGeometryPass()) {
            // Deferred geometry pass: use the GBuffer shader instead of the
            // material's forward shader, but still bind all textures and the
            // material CB so the GBuffer shader can read albedo/roughness/etc.
            context->BindGBufferShader();
            m_material->BindTexturesAndCB();
        } else {
            m_material->Bind();
        }

        // Bind diffuse texture through RenderContext (needed for Vulkan
        // descriptor set switching — OpenGL already binds via Texture::Bind())
        auto* diffuse = m_material->GetDiffuseTexture();
        if (diffuse) {
            context->BindTextureRaw(diffuse, 0);
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
