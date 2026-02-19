#ifndef _COLLIDER_COMPONENT_HPP_
#define _COLLIDER_COMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Physics/Colliders.hpp>

namespace Sleak {

    struct MeshData;

    class ColliderComponent : public Component {
    public:
        // Manual shape constructors
        ColliderComponent(GameObject* owner, const Physics::AABB& aabb);
        ColliderComponent(GameObject* owner, const Physics::BoundingSphere& sphere);
        ColliderComponent(GameObject* owner, const Physics::BoundingCapsule& capsule);

        // Auto-compute from mesh vertices
        ColliderComponent(GameObject* owner, const MeshData& meshData, Physics::ColliderType preferred);

        // Triangle mesh collider
        ColliderComponent(GameObject* owner, const MeshData& meshData, bool asMesh);

        ~ColliderComponent() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;

        const Physics::ColliderShape& GetShape() const { return m_shape; }
        Physics::ColliderType GetType() const { return m_type; }

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
