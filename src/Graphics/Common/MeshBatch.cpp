#include <Runtime/MeshBatch.hpp>
#include "../../include/private/Graphics/Common/ResourceManager.hpp"
#include "../../include/private/Graphics/Common/RenderCommandQueue.hpp"
#include "../../include/private/Graphics/Common/ConstantBuffer.hpp"
#include "../../include/private/Graphics/Common/BufferBase.hpp"
#include <Camera/Camera.hpp>
#include <Runtime/Material.hpp>

namespace Sleak {

// Shared identity transform buffer — allocated once, reused every frame
static RefPtr<RenderEngine::BufferBase> s_batchTransformBuffer;

MeshHandle::MeshHandle() = default;
MeshHandle::~MeshHandle() = default;
MeshHandle::MeshHandle(const MeshHandle&) = default;
MeshHandle::MeshHandle(MeshHandle&&) noexcept = default;
MeshHandle& MeshHandle::operator=(const MeshHandle&) = default;
MeshHandle& MeshHandle::operator=(MeshHandle&&) noexcept = default;

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

MeshHandle MeshBatch::CreateVoxelMesh(VoxelVertexGroup& vertices,
                                       IndexGroup& indices) {
    MeshHandle h;
    if (vertices.GetSize() == 0 || indices.GetSize() == 0) return h;

    auto* vb = RenderEngine::ResourceManager::CreateBuffer(
        RenderEngine::BufferType::Vertex,
        static_cast<uint32_t>(vertices.GetSizeInBytes()),
        vertices.GetRawData());
    if (vb) vb->SetVoxelFormat(true);
    h.vertexBuffer = RefPtr(vb);

    h.indexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
        RenderEngine::BufferType::Index,
        static_cast<uint32_t>(indices.GetByteSize()),
        indices.GetRawData()));

    h.indexCount = static_cast<uint32_t>(indices.GetSize());
    h.isVoxelFormat = true;
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

void MeshBatch::Draw(const MeshHandle& mesh, bool castsShadow) {
    if (!mesh.IsValid()) return;
    RenderEngine::RenderCommandQueue::GetInstance()->SubmitDrawIndexed(
        mesh.vertexBuffer, mesh.indexBuffer, {}, mesh.indexCount, 0, 0,
        castsShadow);
}

void MeshBatch::EndBatch() {
    // Reserved for future use (e.g. restoring render state)
}

void MeshBatch::Shutdown() {
    s_batchTransformBuffer.reset();
}

}
