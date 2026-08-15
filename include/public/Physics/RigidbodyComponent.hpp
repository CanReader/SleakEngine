#ifndef _RIGIDBODY_COMPONENT_HPP_
#define _RIGIDBODY_COMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Core/OSDef.hpp>
#include <Math/Vector.hpp>

namespace Sleak {

    /// How a rigidbody participates in collision response.
    /// @ingroup physics
    enum class BodyType {
        Static,     // Never moves, infinite mass
        Kinematic,  // Moves but controlled by code, full pushback
        Dynamic     // Mass-based response, affected by gravity
    };

    /// Tracks velocity, gravity, and collision state for a GameObject; PhysicsWorld drives its integration.
    ///
    /// Pair this with a ColliderComponent to make an object participate in
    /// simulation. The collider supplies the shape, the rigidbody supplies
    /// the motion state, and Physics::PhysicsWorld integrates and resolves
    /// both. Without a collider, the rigidbody moves but never collides.
    ///
    /// BodyType decides the response. Static bodies never move. Kinematic
    /// bodies move under your control and push others without being
    /// pushed. Dynamic bodies respond by mass and fall under their own
    /// gravity vector, which defaults to standard earth gravity but is
    /// yours to change: platformers and voxel games routinely run heavier
    /// gravity for a snappier feel.
    ///
    /// Gravity is off by default, so call SetUseGravity(true) on anything
    /// that should fall. Collision flags are separated into ground and
    /// wall contacts, which is what a character controller needs to tell
    /// "landed" from "walked into something". Those flags are per-frame:
    /// call ClearCollisionState() once you have consumed them.
    ///
    /// @code{.cpp}
    /// player->AddComponent<Sleak::ColliderComponent>(
    ///     Sleak::Physics::BoundingSphere(
    ///         Sleak::Math::Vector3D(0, 0, 0), 0.3f));
    /// player->AddComponent<Sleak::RigidbodyComponent>(
    ///     Sleak::BodyType::Dynamic);
    ///
    /// auto* rb = player->GetComponent<Sleak::RigidbodyComponent>();
    /// rb->SetUseGravity(true);
    /// rb->SetGravity(Sleak::Math::Vector3D(0.0f, -32.0f, 0.0f));
    /// rb->SetMass(80.0f);
    /// rb->SetTerminalVelocity(60.0f);
    ///
    /// // Later, in an update
    /// if (rb->IsGrounded() && jumpPressed) {
    ///     Sleak::Math::Vector3D v = rb->GetVelocity();
    ///     v.SetY(6.0f);
    ///     rb->SetVelocity(v);
    /// }
    /// if (rb->HadWallCollision()) {
    ///     SlideAlong(rb->GetWallNormal());
    /// }
    /// rb->ClearCollisionState();
    /// @endcode
    ///
    /// @see ColliderComponent, Physics::PhysicsWorld, BodyType,
    ///      FirstPersonController
    /// @ingroup physics
    class ENGINE_API RigidbodyComponent : public Component {
    public:
        RigidbodyComponent(GameObject* owner, BodyType type = BodyType::Kinematic);
        ~RigidbodyComponent() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;

        /// Pushes the body out along normal by penetration and updates ground/wall collision state.
        void ResolveCollision(const Math::Vector3D& normal, float penetration);

        BodyType GetBodyType() const { return m_bodyType; }
        void SetBodyType(BodyType type) { m_bodyType = type; }

        // General collision state (any collision this frame)
        bool HadCollision() const { return m_hadCollision; }
        Math::Vector3D GetLastCollisionNormal() const { return m_lastCollisionNormal; }

        // Separate ground vs wall collision tracking
        bool HadGroundCollision() const { return m_hadGroundCollision; }
        Math::Vector3D GetGroundNormal() const { return m_groundNormal; }
        bool HadWallCollision() const { return m_hadWallCollision; }
        Math::Vector3D GetWallNormal() const { return m_wallNormal; }

        /// Resets HadCollision/HadGroundCollision/HadWallCollision for the next frame.
        void ClearCollisionState();

        // Velocity
        Math::Vector3D GetVelocity() const { return m_velocity; }
        void SetVelocity(const Math::Vector3D& velocity) { m_velocity = velocity; }
        void AddForce(const Math::Vector3D& force) { m_velocity = m_velocity + force * (1.0f / m_mass); }

        // Gravity
        Math::Vector3D GetGravity() const { return m_gravity; }
        void SetGravity(const Math::Vector3D& gravity) { m_gravity = gravity; }
        bool GetUseGravity() const { return m_useGravity; }
        void SetUseGravity(bool useGravity) { m_useGravity = useGravity; }

        // Grounded state
        bool IsGrounded() const { return m_isGrounded; }
        void SetGrounded(bool grounded) { m_isGrounded = grounded; }

        // Mass
        float GetMass() const { return m_mass; }
        void SetMass(float mass) { m_mass = mass > 0.0f ? mass : 1.0f; }

        // Terminal velocity
        float GetTerminalVelocity() const { return m_terminalVelocity; }
        void SetTerminalVelocity(float tv) { m_terminalVelocity = tv; }

    private:
        BodyType m_bodyType;

        // Collision tracking
        Math::Vector3D m_lastCollisionNormal;
        bool m_hadCollision = false;

        Math::Vector3D m_groundNormal = Math::Vector3D(0, 1, 0);
        bool m_hadGroundCollision = false;

        Math::Vector3D m_wallNormal;
        bool m_hadWallCollision = false;

        // Physics state
        Math::Vector3D m_velocity = Math::Vector3D(0, 0, 0);
        Math::Vector3D m_gravity = Math::Vector3D(0, -9.81f, 0);
        bool m_useGravity = false;
        bool m_isGrounded = false;
        float m_mass = 1.0f;
        float m_terminalVelocity = 50.0f;
    };

} // namespace Sleak

#endif // _RIGIDBODY_COMPONENT_HPP_
