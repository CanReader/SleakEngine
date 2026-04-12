#include <Runtime/MeshBatch.hpp>
#include "../../include/private/Graphics/ResourceManager.hpp"
#include "../../include/private/Graphics/RenderCommandQueue.hpp"
#include "../../include/private/Graphics/ConstantBuffer.hpp"
#include "../../include/private/Graphics/BufferBase.hpp"
#include <Camera/Camera.hpp>
#include <Runtime/Material.hpp>

namespace Sleak {

// Shared identity transform buffer — allocated once, reused every frame
static RefPtr<RenderEngine::BufferBase> s_batchTransformBuffer;

MeshHandle MeshBatch::CreateMesh(VertexGroup& vertices, IndexGroup& indices) {
    MeshHandle h;
    if (vertices.GetSize() == 0 || indices.GetSize() == 0) return h;

    h.vertexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
        RenderEngine::BufferType::Vertex,
        static_cast<uint32_t>(vertices.GetSizeInBytes()),
        vertices.GetRawData()));

    h.indexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
        RenderEngine::BufferType::Index,
        static_cast<uint32_t>(indices.GetByteSize()),
        indices.GetRawData()));

    h.indexCount = static_cast<uint32_t>(indices.GetSize());
    return h;
}

void MeshBatch::BeginBatch(Material* material) {
    auto* queue = RenderEngine::RenderCommandQueue::GetInstance();

    // Bind material once for all draws in this batch
    queue->SubmitBindMaterial(material);

    // Build identity world matrix — chunk vertices are already in world space
    auto world = Math::Matrix4::Identity();
    RenderEngine::TransformBuffer tb(world,
                                     Camera::GetMainViewMatrix(),
                                     Camera::GetMainProjectionMatrix());

    if (!s_batchTransformBuffer) {
        s_batchTransformBuffer = RefPtr<RenderEngine::BufferBase>(
            RenderEngine::ResourceManager::CreateBuffer(
                RenderEngine::BufferType::Constant,
                tb.GetSize(), tb.GetData()));
    } else {
        queue->SubmitUpdateConstantBuffer(
            s_batchTransformBuffer, tb.GetData(), tb.GetSize());
    }

    queue->SubmitBindConstantBuffer(s_batchTransformBuffer, 0);
}

void MeshBatch::Draw(const MeshHandle& mesh) {
    if (!mesh.IsValid()) return;
    RenderEngine::RenderCommandQueue::GetInstance()->SubmitDrawIndexed(
        mesh.vertexBuffer, mesh.indexBuffer, {}, mesh.indexCount);
}

void MeshBatch::EndBatch() {
    // Reserved for future use (e.g. restoring render state)
}

void MeshBatch::Shutdown() {
    s_batchTransformBuffer.reset();
}

}
