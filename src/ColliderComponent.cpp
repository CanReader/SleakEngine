#include <Physics/ColliderComponent.hpp>
#include <Core/GameObject.hpp>
#include <Camera/Camera.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include "../../include/private/Graphics/Vertex.hpp"

namespace Sleak {

ColliderComponent::ColliderComponent(GameObject* owner, const Physics::AABB& aabb)
    : Component(owner), m_shape(aabb), m_type(Physics::ColliderType::AABB) {}

ColliderComponent::ColliderComponent(GameObject* owner, const Physics::BoundingSphere& sphere)
    : Component(owner), m_shape(sphere), m_type(Physics::ColliderType::Sphere) {}

ColliderComponent::ColliderComponent(GameObject* owner, const Physics::BoundingCapsule& capsule)
    : Component(owner), m_shape(capsule), m_type(Physics::ColliderType::Capsule) {}

ColliderComponent::ColliderComponent(GameObject* owner, const MeshData& meshData, Physics::ColliderType preferred)
    : Component(owner) {
    // Compute AABB from mesh vertices
    const Vertex* verts = meshData.vertices.GetData();
    size_t count = meshData.vertices.GetSize();

    Physics::AABB bounds = Physics::AABB::FromVertices(
        &verts[0].px, count, sizeof(Vertex));

    switch (preferred) {
        case Physics::ColliderType::Sphere: {
            // Compute tight bounding sphere from vertices (not from AABB corners)
            Math::Vector3D center = bounds.GetCenter();
            float maxDistSq = 0.0f;
            for (size_t i = 0; i < count; ++i) {
                Math::Vector3D v(verts[i].px, verts[i].py, verts[i].pz);
                Math::Vector3D diff = v - center;
                float dSq = diff.Dot(diff);
                if (dSq > maxDistSq) maxDistSq = dSq;
            }
            m_shape = Physics::BoundingSphere(
                Physics::Vector3D(center.GetX(), center.GetY(), center.GetZ()),
                std::sqrt(maxDistSq));
            m_type = Physics::ColliderType::Sphere;
            break;
        }
        case Physics::ColliderType::Capsule: {
            m_shape = Physics::BoundingCapsule::FromAABB(bounds);
            m_type = Physics::ColliderType::Capsule;
            break;
        }
        default: {
            m_shape = bounds;
            m_type = Physics::ColliderType::AABB;
            break;
        }
    }
}

ColliderComponent::ColliderComponent(GameObject* owner, const MeshData& meshData, bool asMesh)
    : Component(owner) {
    if (asMesh) {
        const Vertex* verts = meshData.vertices.GetData();
        size_t vertCount = meshData.vertices.GetSize();
        const IndexType* indices = meshData.indices.GetData();
        size_t indexCount = meshData.indices.GetSize();

        Physics::TriangleMesh mesh;
        mesh.Build(&verts[0].px, vertCount, sizeof(Vertex),
                   indices, indexCount);
        m_shape = std::move(mesh);
        m_type = Physics::ColliderType::Mesh;
    } else {
        const Vertex* verts = meshData.vertices.GetData();
        size_t count = meshData.vertices.GetSize();
        m_shape = Physics::AABB::FromVertices(&verts[0].px, count, sizeof(Vertex));
        m_type = Physics::ColliderType::AABB;
    }
}

bool ColliderComponent::Initialize() {
    bIsInitialized = true;
    return true;
}

void ColliderComponent::Update(float /*deltaTime*/) {
    // Collider shape is static relative to owner - no per-frame work needed
}

Physics::AABB ColliderComponent::GetWorldAABB() const {
    Math::Vector3D worldPos;
    Math::Vector3D worldScale(1, 1, 1);

    auto* transform = const_cast<GameObject*>(owner)->GetComponent<TransformComponent>();
    if (transform) {
        worldPos = transform->GetWorldPosition() + m_offset;
        worldScale = transform->GetWorldScale();
    } else if (auto* cam = dynamic_cast<Camera*>(owner)) {
        worldPos = cam->GetPosition() + m_offset;
    }

    return Physics::GetWorldAABB(m_shape, worldPos, worldScale);
}

} // namespace Sleak
