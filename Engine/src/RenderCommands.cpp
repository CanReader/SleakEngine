#include "../../include/private/Graphics/RenderContext.hpp"
#include "../../include/private/Graphics/RenderCommands.hpp"
#include "../../include/private/Graphics/BufferBase.hpp"
#include "../../include/private/Graphics/Renderer.hpp"
#include "../../include/private/Graphics/ConstantBuffer.hpp"
#include "../../include/private/Graphics/Shader.hpp"
#include "../../include/public/Runtime/Texture.hpp"

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
        if (b) context->BindConstantBuffer(b);

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

    for (const auto& b : m_constantBuffers)
        if (b) context->BindConstantBuffer(b, 0);

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
