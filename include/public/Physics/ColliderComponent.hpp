#ifndef _COLLIDER_COMPONENT_HPP_
#define _COLLIDER_COMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Core/OSDef.hpp>
#include <Physics/Colliders.hpp>

namespace Sleak {

    struct MeshData;

    /// Attaches a collision shape to a GameObject and registers it with PhysicsWorld's broadphase.
    ///
    /// Attach the shape that matches the object: Physics::AABB for boxes
    /// and level geometry, Physics::BoundingSphere for projectiles and
    /// simple characters, Physics::BoundingCapsule for upright figures.
    /// Two more constructors derive a shape from MeshData, either a
    /// bounding volume of a type you pick or an exact triangle mesh.
    ///
    /// Registration is automatic. Adding the owning GameObject to a scene
    /// registers the collider (and any on child objects) with that scene's
    /// Physics::PhysicsWorld, and removing the object unregisters it. The
    /// shape you supply is in local space; GetWorldAABB() applies the
    /// owner's current transform.
    ///
    /// Layer and mask filter both collision response and the world query
    /// API, so put triggers, terrain, and characters on distinct layers and
    /// a raycast can ignore the ones it does not care about. Marking a
    /// collider as a trigger keeps it in the broadphase while skipping
    /// physical response.
    ///
    /// @code{.cpp}
    /// // Static level geometry
    /// floor->AddComponent<Sleak::ColliderComponent>(
    ///     Sleak::Physics::AABB(
    ///         Sleak::Math::Vector3D(-0.5f, -0.5f, -0.5f),
    ///         Sleak::Math::Vector3D( 0.5f,  0.5f,  0.5f)));
    /// floor->AddComponent<Sleak::RigidbodyComponent>(
    ///     Sleak::BodyType::Static);
    /// AddObject(floor);      // registers with the scene's PhysicsWorld
    ///
    /// // An upright character, raised to sit on the ground
    /// player->AddComponent<Sleak::ColliderComponent>(
    ///     Sleak::Physics::BoundingCapsule(
    ///         Sleak::Math::Vector3D(0, 0, 0), 0.35f, 0.9f));
    ///
    /// // A pickup volume that reports overlaps but never pushes
    /// if (auto* c = pickup->GetComponent<Sleak::ColliderComponent>()) {
    ///     c->SetTrigger(true);
    ///     c->SetLayer(LAYER_PICKUP);
    /// }
    /// @endcode
    ///
    /// @see RigidbodyComponent, Physics::PhysicsWorld, Physics::AABB,
    ///      Physics::BoundingSphere, Physics::BoundingCapsule
    /// @ingroup physics
    class ENGINE_API ColliderComponent : public Component {
    public:
        /// Wraps a pre-built axis-aligned box shape.
        ColliderComponent(GameObject* owner, const Physics::AABB& aabb);
        /// Wraps a pre-built sphere shape.
        ColliderComponent(GameObject* owner, const Physics::BoundingSphere& sphere);
        /// Wraps a pre-built capsule shape.
        ColliderComponent(GameObject* owner, const Physics::BoundingCapsule& capsule);

        /// Derives a bounding shape of the given type from the mesh's vertex positions.
        ColliderComponent(GameObject* owner, const MeshData& meshData, Physics::ColliderType preferred);

        /// Builds an exact triangle-mesh collider when asMesh is true;
        /// otherwise falls back to an AABB computed from the mesh's vertices.
        ColliderComponent(GameObject* owner, const MeshData& meshData, bool asMesh);

        ~ColliderComponent() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;

        const Physics::ColliderShape& GetShape() const { return m_shape; }
        Physics::ColliderType GetType() const { return m_type; }

        /// Local shape transformed into world space by the owner's current transform.
        Physics::AABB GetWorldAABB() const;

        // Offset from owner's transform
        void SetOffset(const Math::Vector3D& offset) { m_offset = offset; }
        Math::Vector3D GetOffset() const { return m_offset; }

        // Layer/mask for filtering
        void SetLayer(uint32_t layer) { m_layer = layer; }
        uint32_t GetLayer() const { return m_layer; }
        void SetMask(uint32_t mask) { m_mask = mask; }
        uint32_t GetMask() const { return m_mask; }

        // Trigger (no physics response, just events)
        void SetTrigger(bool trigger) { m_isTrigger = trigger; }
        bool IsTrigger() const { return m_isTrigger; }

        // Broadphase proxy
        void SetProxyId(int id) { m_proxyId = id; }
        int GetProxyId() const { return m_proxyId; }

    private:
        Physics::ColliderShape m_shape;
        Physics::ColliderType m_type = Physics::ColliderType::AABB;
        Math::Vector3D m_offset;
        uint32_t m_layer = 0xFFFFFFFF;
        uint32_t m_mask = 0xFFFFFFFF;
        bool m_isTrigger = false;
        int m_proxyId = -1;
    };

} // namespace Sleak

#endif // _COLLIDER_COMPONENT_HPP_
