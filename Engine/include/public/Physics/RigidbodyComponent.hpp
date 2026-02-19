#ifndef _RIGIDBODY_COMPONENT_HPP_
#define _RIGIDBODY_COMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Math/Vector.hpp>

namespace Sleak {

    enum class BodyType {
        Static,     // Never moves, infinite mass
        Kinematic,  // Moves but controlled by code, full pushback
        Dynamic     // Future: mass-based response
    };

    class RigidbodyComponent : public Component {
    public:
        RigidbodyComponent(GameObject* owner, BodyType type = BodyType::Kinematic);
        ~RigidbodyComponent() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;

        void ResolveCollision(const Math::Vector3D& normal, float penetration);

        BodyType GetBodyType() const { return m_bodyType; }
        void SetBodyType(BodyType type) { m_bodyType = type; }

        bool HadCollision() const { return m_hadCollision; }
        Math::Vector3D GetLastCollisionNormal() const { return m_lastCollisionNormal; }
        void ClearCollisionState() { m_hadCollision = false; m_lastCollisionNormal = Math::Vector3D::Zero(); }

    private:
        BodyType m_bodyType;
        Math::Vector3D m_lastCollisionNormal;
        bool m_hadCollision = false;
    };

} // namespace Sleak

#endif // _RIGIDBODY_COMPONENT_HPP_
