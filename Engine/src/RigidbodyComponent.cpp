#include <Physics/RigidbodyComponent.hpp>
#include <Core/GameObject.hpp>
#include <Camera/Camera.hpp>
#include <ECS/Components/TransformComponent.hpp>

namespace Sleak {

RigidbodyComponent::RigidbodyComponent(GameObject* owner, BodyType type)
    : Component(owner), m_bodyType(type) {}

bool RigidbodyComponent::Initialize() {
    bIsInitialized = true;
    return true;
}

void RigidbodyComponent::Update(float /*deltaTime*/) {
    // Future: integrate velocity, gravity, etc.
}

void RigidbodyComponent::ResolveCollision(const Math::Vector3D& normal, float penetration) {
    if (m_bodyType == BodyType::Static) return;

    m_lastCollisionNormal = normal;
    m_hadCollision = true;

    Math::Vector3D correction = normal * penetration;

    auto* transform = owner->GetComponent<TransformComponent>();
    if (transform) {
        transform->Translate(correction);
        return;
    }

    // Camera doesn't use TransformComponent — push via Camera::AddPosition
    if (auto* cam = dynamic_cast<Camera*>(owner)) {
        cam->AddPosition(correction);
    }
}

} // namespace Sleak
