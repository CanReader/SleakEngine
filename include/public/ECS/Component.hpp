#ifndef _COMPONENT_H_
#define _COMPONENT_H_

#include <Core/Object.hpp>
#include <Memory/RefPtr.hpp>
#include <Core/Logger.hpp>

namespace Sleak {
    class GameObject;
    /// Base for behavior attached to a GameObject. Owner deletes it via
    /// GameObject::RemoveComponent(), never directly.
    ///
    /// Derive from Component to add your own behavior to an object. The
    /// constructor must take the owning `GameObject*` as its first
    /// parameter, because GameObject::AddComponent() supplies it and
    /// forwards the rest. Initialize() and Update() are pure virtual;
    /// FixedUpdate(), LateUpdate(), OnEnable(), OnDisable(), and
    /// OnDestroy() are optional.
    ///
    /// The owning GameObject holds components in a RefPtr and destroys them
    /// with itself, so never delete a component directly. Reach the owner
    /// through GetOwner() to find sibling components.
    ///
    /// Lifecycle: the object calls Initialize() once (immediately if the
    /// object is already initialized when the component is attached, and
    /// otherwise during the object's own Initialize()), then OnEnable() if
    /// the object is active, then Update() every frame.
    ///
    /// @code{.cpp}
    /// class SpinComponent : public Sleak::Component {
    /// public:
    ///     SpinComponent(Sleak::GameObject* owner, float degreesPerSecond)
    ///         : Sleak::Component(owner), m_speed(degreesPerSecond) {}
    ///
    ///     bool Initialize() override {
    ///         m_transform = GetOwner()
    ///             ->GetComponent<Sleak::TransformComponent>();
    ///         return m_transform != nullptr;
    ///     }
    ///
    ///     void Update(float deltaTime) override {
    ///         if (!m_transform) return;
    ///         m_transform->RotateAround(
    ///             Sleak::Math::Vector3D(0.0f, 1.0f, 0.0f),
    ///             m_speed * deltaTime);
    ///     }
    ///
    /// private:
    ///     float m_speed;
    ///     Sleak::TransformComponent* m_transform = nullptr;
    /// };
    ///
    /// // Attach it; 90.0f is forwarded to the constructor
    /// obj->AddComponent<SpinComponent>(90.0f);
    /// @endcode
    ///
    /// @see GameObject, TransformComponent, CameraController
    /// @ingroup scene
    class Component : public Object {
        public:
            Component(GameObject* object) : bIsInitialized(false), owner(object) {}
            virtual ~Component() = default;

            /// One-time setup, called after the component is attached and the owner is initialized.
            virtual bool Initialize() = 0;

            virtual void Update(float deltaTime) = 0;
            virtual void FixedUpdate(float fixedDeltaTime) {}
            virtual void LateUpdate(float deltaTime) {}

            /// Called when the component is detached or the owner is destroyed.
            virtual void OnDestroy() {}
            virtual void OnEnable() {}
            virtual void OnDisable() {}

            GameObject* GetOwner() {
                return owner;
            }

        protected:
            GameObject* owner;
            bool bIsInitialized;

    };
}

#endif