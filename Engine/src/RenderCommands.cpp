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

//------------------------------------------------------------------------------
// Drawing
//------------------------------------------------------------------------------

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
    // Transform buffer (slot 0) is bound by ExecuteShadowPass before this call
    context->Draw(m_vertexCount);
}

//------------------------------------------------------------------------------
// Indexed Drawing
//------------------------------------------------------------------------------

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
    // Transform buffer (slot 0) is bound by ExecuteShadowPass before this call
    context->DrawIndexed(m_indexCount);
}

//------------------------------------------------------------------------------
// Update Constant Buffer
//------------------------------------------------------------------------------

UpdateConstantBufferCommand::UpdateConstantBufferCommand(RefPtr<BufferBase> buffer,
    void* Data,
    uint16_t Size) : 
      constantBuffer(buffer), 
      Data(nullptr),
      Size(Size) {
    this->Data = malloc(Size);

    if (this->Data) {
        memcpy(this->Data,Data,Size);
    } else {
        SLEAK_ERROR("Failed to allocate memory at UpdateConstantBufferCommand!");
    }
}

void UpdateConstantBufferCommand::Execute(RenderContext* context) {
    constantBuffer->Update(Data,Size);
}

//------------------------------------------------------------------------------
// Bind Constant Buffer
//------------------------------------------------------------------------------

BindConstantBufferCommand::BindConstantBufferCommand(RefPtr<BufferBase> buffer, int slot) : 
      constantBuffer(buffer), slot(slot) {}

void BindConstantBufferCommand::Execute(RenderContext* context) {
    context->BindConstantBuffer(constantBuffer, slot);
}

//------------------------------------------------------------------------------
// Set Render Mode
//------------------------------------------------------------------------------

SetRenderModeCommand::SetRenderModeCommand(RenderMode mode) : mode(mode) {}

void SetRenderModeCommand::Execute(RenderContext* context) {
    context->SetRenderMode(mode);
}

//------------------------------------------------------------------------------
// Set Render Face
//------------------------------------------------------------------------------

SetRenderFaceCommand::SetRenderFaceCommand(RenderFace face) : face(face) {}

void SetRenderFaceCommand::Execute(RenderContext* context) {
    context->SetRenderFace(face);
}

//------------------------------------------------------------------------------
// Bind Material
//------------------------------------------------------------------------------

BindMaterialCommand::BindMaterialCommand(::Sleak::Material* material)
    : m_material(material) {}

void BindMaterialCommand::Execute(RenderContext* context) {
    if (m_material) {
        m_material->Bind();

        // Bind diffuse texture through RenderContext (needed for Vulkan
        // descriptor set switching — OpenGL already binds via Texture::Bind())
        auto* diffuse = m_material->GetDiffuseTexture();
        if (diffuse) {
            context->BindTextureRaw(diffuse, 0);
        }
    }
}

//------------------------------------------------------------------------------
// Custom Command
//------------------------------------------------------------------------------

CustomCommand::CustomCommand(ExecuteFunction function)
    : m_executeFunction(function) {}

void CustomCommand::Execute(RenderContext* context) { 
    if (m_executeFunction)
        m_executeFunction(context);
}
    
}

}
