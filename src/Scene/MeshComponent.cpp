#include "../../include/private/Graphics/Common/ResourceManager.hpp"
#include "../../include/private/Graphics/Common/BufferBase.hpp"
#include "../../include/private/Graphics/Common/ConstantBuffer.hpp"
#include "../../include/private/Graphics/Common/RenderCommandQueue.hpp"
#include <ECS/Components/MeshComponent.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Core/GameObject.hpp>
#include <Culling/CullingSystem.hpp>
#include <Runtime/MeshData.hpp>

namespace Sleak {

namespace {
    // World point (row-vector convention: world = local * M) as a
    // degenerate AABB for accumulation.
    Math::AABB TransformedPoint(const Math::Vector3D& p,
                                const Math::Matrix4& m) {
        float x = p.GetX(), y = p.GetY(), z = p.GetZ();
        Math::Vector3D w(
            x * m(0, 0) + y * m(1, 0) + z * m(2, 0) + m(3, 0),
            x * m(0, 1) + y * m(1, 1) + z * m(2, 1) + m(3, 1),
            x * m(0, 2) + y * m(1, 2) + z * m(2, 2) + m(3, 2));
        return Math::AABB(w, w);
    }
}  // namespace

MeshComponent::MeshComponent(GameObject* object, MeshData data) : Component(object) {
        VertexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
            RenderEngine::BufferType::Vertex,
            data.vertices.GetSizeInBytes(),
            data.vertices.GetRawData()));

        IndexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
            RenderEngine::BufferType::Index,
            data.indices.GetByteSize(),
            data.indices.GetRawData()));

            VertexCount = data.vertices.GetSize();
            IndexCount = data.indices.GetSize();
    }

MeshComponent::MeshComponent(GameObject* object, VoxelMeshData data) : Component(object) {
        auto* vb = RenderEngine::ResourceManager::CreateBuffer(
            RenderEngine::BufferType::Vertex,
            data.vertices.GetSizeInBytes(),
            data.vertices.GetRawData());
        if (vb) vb->SetVoxelFormat(true);
        VertexBuffer = RefPtr(vb);

        IndexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
            RenderEngine::BufferType::Index,
            data.indices.GetByteSize(),
            data.indices.GetRawData()));

            VertexCount = data.vertices.GetSize();
            IndexCount = data.indices.GetSize();
    }

    bool MeshComponent::Initialize() {
        if (!VertexBuffer.IsValid())
            return false;
        
        if (!IndexBuffer.IsValid())
            return false;
                
        bIsInitialized = true;

        return true;
    }

    void MeshComponent::Update(float deltaTime) {
        if (!bIsInitialized)
            return;

        if (m_hasLocalBounds || m_hasCullBounds) {
            Math::AABB worldBounds = m_cullBounds;
            bool cullReady = m_hasCullBounds;

            if (m_hasLocalBounds && owner) {
                if (auto* tc = owner->GetComponent<TransformComponent>()) {
                    Math::Matrix4 m = tc->GetTransformMatrix();
                    Math::AABB acc = TransformedPoint(m_localBounds.Corner(0), m);
                    for (int i = 1; i < 8; ++i)
                        acc.Merge(TransformedPoint(m_localBounds.Corner(i), m));
                    worldBounds = acc;
                    cullReady = true;
                }
            }

            if (cullReady &&
                !CullingSystem::IsVisibleFrustumOnly(worldBounds))
                return;
        }

        RenderEngine::RenderCommandQueue::GetInstance()->SubmitDrawIndexed(VertexBuffer,IndexBuffer,ConstantBuffers,IndexCount);
    }

    void MeshComponent::SetVertexBuffer(RefPtr<RenderEngine::BufferBase>& buffer) {
        this->VertexBuffer = buffer;
    }
    
    void MeshComponent::SetIndexBuffer(RefPtr<RenderEngine::BufferBase>& buffer) {
        this->IndexBuffer = buffer;
    }

    void MeshComponent::AddConstantBuffer(RenderEngine::TransformBuffer& bufferData) {
        auto buffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
            RenderEngine::BufferType::Constant, sizeof(RenderEngine::TransformBuffer),
            &bufferData));

        ConstantBuffers.add(buffer);
    }

    void MeshComponent::AddConstantBuffer(
        RefPtr<RenderEngine::BufferBase>& buffer) {
        ConstantBuffers.add(buffer);
    }

    void MeshComponent::SetCullBoundsWorld(const Math::AABB& bounds) {
        m_cullBounds = bounds;
        m_hasCullBounds = true;
        m_hasLocalBounds = false;
    }

    void MeshComponent::SetCullBoundsLocal(const Math::AABB& bounds) {
        m_localBounds = bounds;
        m_hasLocalBounds = true;
    }

    }