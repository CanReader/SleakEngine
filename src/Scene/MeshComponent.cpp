#include <Core/GameObject.hpp>
#include <Culling/CullingSystem.hpp>
#include <ECS/Components/MaterialComponent.hpp>
#include <ECS/Components/MeshComponent.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Runtime/MeshData.hpp>
#include <cmath>

#include "../../include/private/Graphics/Common/BufferBase.hpp"
#include "../../include/private/Graphics/Common/ConstantBuffer.hpp"
#include "../../include/private/Graphics/Common/RenderCommandQueue.hpp"
#include "../../include/private/Graphics/Common/ResourceManager.hpp"

namespace Sleak {

namespace {
/// World AABB of a local box under m (world = local * m), via Arvo's method.
Math::AABB TransformBounds(const Math::AABB& box, const Math::Matrix4& m) {
    const Math::Vector3D c = box.Center();
    const Math::Vector3D e = box.Extents();
    const float cx = c.GetX(), cy = c.GetY(), cz = c.GetZ();
    const float ex = std::fabs(e.GetX());
    const float ey = std::fabs(e.GetY());
    const float ez = std::fabs(e.GetZ());

    const Math::Vector3D center(
        cx * m(0, 0) + cy * m(1, 0) + cz * m(2, 0) + m(3, 0),
        cx * m(0, 1) + cy * m(1, 1) + cz * m(2, 1) + m(3, 1),
        cx * m(0, 2) + cy * m(1, 2) + cz * m(2, 2) + m(3, 2));
    const Math::Vector3D extents(
        ex * std::fabs(m(0, 0)) + ey * std::fabs(m(1, 0)) +
            ez * std::fabs(m(2, 0)),
        ex * std::fabs(m(0, 1)) + ey * std::fabs(m(1, 1)) +
            ez * std::fabs(m(2, 1)),
        ex * std::fabs(m(0, 2)) + ey * std::fabs(m(1, 2)) +
            ez * std::fabs(m(2, 2)));
    return Math::AABB(center - extents, center + extents);
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

        TransformComponent* tc =
            owner ? owner->GetComponent<TransformComponent>() : nullptr;

        if (m_hasLocalBounds || m_hasCullBounds) {
            Math::AABB worldBounds = m_cullBounds;
            bool cullReady = m_hasCullBounds;

            if (m_hasLocalBounds && tc) {
                worldBounds =
                    TransformBounds(m_localBounds, tc->GetTransformMatrix());
                cullReady = true;
            }

            if (cullReady &&
                !CullingSystem::IsVisibleFrustumOnly(worldBounds))
                return;
        }

        if (tc) tc->SubmitRenderState();
        if (MaterialComponent* mc =
                owner ? owner->GetComponent<MaterialComponent>() : nullptr)
            mc->SubmitRenderState();

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