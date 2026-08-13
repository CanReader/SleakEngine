#ifndef _COMPONENT_H_
#define _COMPONENT_H_

#include <Core/Object.hpp>
#include <Memory/RefPtr.hpp>
#include <Core/Logger.hpp>

namespace Sleak {
    class GameObject;
    /// Base for behavior attached to a GameObject. Owner deletes it via
    /// GameObject::RemoveComponent(), never directly.
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