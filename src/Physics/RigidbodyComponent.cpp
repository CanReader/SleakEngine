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
    // Gravity integration is handled by PhysicsWorld::Step
}

void RigidbodyComponent::ClearCollisionState() {
    m_hadCollision = false;
    m_lastCollisionNormal = Math::Vector3D::Zero();
    m_hadGroundCollision = false;
    m_hadWallCollision = false;
    m_wallNormal = Math::Vector3D::Zero();
    // Note: m_groundNormal is NOT cleared — keep last known ground normal
}

void RigidbodyComponent::ResolveCollision(const Math::Vector3D& normal, float penetration) {
    if (m_bodyType == BodyType::Static) return;

    m_lastCollisionNormal = normal;
    m_hadCollision = true;

    // Position correction — push out of the colliding object
    Math::Vector3D correction = normal * penetration;

    auto* transform = owner->GetComponent<TransformComponent>();
    if (transform) {
        transform->Translate(correction);
    } else if (auto* cam = dynamic_cast<Camera*>(owner)) {
        cam->AddPosition(correction);
    }

    if (m_bodyType != BodyType::Dynamic) return;

    // Classify collision: ground (normal.y > 0.7) vs wall
    bool isGroundCollision = normal.GetY() > 0.7f;

    if (isGroundCollision) {
        m_hadGroundCollision = true;
        m_groundNormal = normal;
        m_isGrounded = true;

        // Zero out downward velocity when landing on ground
        if (m_velocity.GetY() < 0.0f) {
            m_velocity = Math::Vector3D(m_velocity.GetX(), 0.0f, m_velocity.GetZ());
        }
    } else {
        m_hadWallCollision = true;
        m_wallNormal = normal;

        // Cancel velocity component pushing INTO the wall
        float dot = m_velocity.Dot(normal);
        if (dot < 0.0f) {
            m_velocity = m_velocity - normal * dot;
        }
    }
}

} // namespace Sleak
