#ifndef _COMPONENT_H_
#define _COMPONENT_H_

#include <Core/Object.hpp>
#include <Memory/RefPtr.h>
#include <Logger.hpp>

namespace Sleak {
    class GameObject;
    class Component : public Object {
        public:
            Component(GameObject* object) : bIsInitialized(false), owner(object) {}
            virtual ~Component() = default;

            virtual bool Initialize() = 0;

            virtual void Update(float deltaTime) = 0;
            virtual void FixedUpdate(float fixedDeltaTime) {}
            virtual void LateUpdate(float deltaTime) {}

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