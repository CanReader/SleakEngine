#include <Physics/ColliderComponent.hpp>
#include <Physics/RigidbodyComponent.hpp>
#include <Core/GameObject.hpp>
#include <Camera/Camera.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Runtime/MeshData.hpp>

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

void ColliderComponent::RefreshCache() const {
    if (!owner) return;
    uint32_t version = owner->GetComponentVersion();
    if (version == m_cacheVersion) return;

    m_body = owner->GetComponent<RigidbodyComponent>();
    m_transform = owner->GetComponent<TransformComponent>();
    m_camera = m_transform ? nullptr : dynamic_cast<Camera*>(owner);
    m_cacheVersion = version;
}

RigidbodyComponent* ColliderComponent::GetRigidbody() const {
    RefreshCache();
    return m_body;
}

void ColliderComponent::GetWorldPose(Math::Vector3D& position,
                                     Math::Vector3D& scale) const {
    RefreshCache();
    position = m_offset;
    scale = Math::Vector3D(1, 1, 1);
    if (m_transform) {
        position = m_transform->GetWorldPosition() + m_offset;
        scale = m_transform->GetWorldScale();
    } else if (m_camera) {
        position = m_camera->GetPosition() + m_offset;
    }
}

void ColliderComponent::TranslateOwner(const Math::Vector3D& delta) {
    RefreshCache();
    if (m_transform) {
        m_transform->Translate(delta);
    } else if (m_camera) {
        m_camera->AddPosition(delta);
    }
}

Physics::AABB ColliderComponent::GetWorldAABB() const {
    Math::Vector3D worldPos;
    Math::Vector3D worldScale;
    GetWorldPose(worldPos, worldScale);
    return Physics::GetWorldAABB(m_shape, worldPos, worldScale);
}

} // namespace Sleak
