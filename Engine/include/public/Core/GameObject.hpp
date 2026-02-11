#ifndef _GAMEOBJECT_H_
#define _GAMEOBJECT_H_

#include "Object.hpp"
#include <Logger.hpp>
#include <ECS/Component.hpp>
#include <Utility/Container/List.hpp>
#include <Memory/RefPtr.h>
#include <type_traits>
#include <string>

namespace Sleak {
    namespace Math {class Vector3D;};
    class ENGINE_API GameObject : public Object {
    public:
        GameObject(const std::string& name = "GameObject")
            : Object(name), m_isActive(true), bIsInitialized(false),
              m_pendingDestroy(false), m_parent(nullptr) {}

        ~GameObject() override;

        // --- Component management ---

        template<typename T, typename... Args>
        void AddComponent(Args&&... args) {
            static_assert(std::is_base_of<Component, T>::value, "T must derive from Component!");

            if (GetComponent<T>() != nullptr) {
                SLEAK_WARN("The component already exists!");
                return;
            }

            RefPtr<Component> newComponent = RefPtr<T>(new T(this, std::forward<Args>(args)...));
            if (bIsInitialized) newComponent->Initialize();
            if (m_isActive && bIsInitialized) newComponent->OnEnable();
            Components.add(std::move(newComponent));
        }

        template<typename T>
        void RemoveComponent() {
            static_assert(std::is_base_of<Component, T>::value, "T must derive from Component!");

            for (size_t i = 0; i < Components.GetSize(); ++i) {
                if (dynamic_cast<T*>(Components[i].get()) != nullptr) {
                    Components[i]->OnDestroy();
                    Components.erase(i);
                    break;
                }
            }
        }

        template <typename T>
        T* GetComponent() {
            static_assert(std::is_base_of_v<Component, T>,
                          "T must derive from Component!");

            for (size_t i = 0; i < Components.GetSize(); ++i) {
                Component* rawPtr = Components[i].get();
                if (!rawPtr) continue;

                T* component = dynamic_cast<T*>(rawPtr);
                if (component) return component;
            }

            return nullptr;
        }

        template <typename T>
        bool HasComponent() {
            static_assert(std::is_base_of_v<Component, T>,
                          "T must derive from Component!");
            return GetComponent<T>() != nullptr;
        }

        // --- Lifecycle ---

        virtual void Initialize();
        virtual void Update(float deltaTime);
        virtual void FixedUpdate(float fixedDeltaTime);
        virtual void LateUpdate(float deltaTime);

        void SetActive(bool active);
        bool IsActive() const { return m_isActive; }

        // --- Tag system ---

        void SetTag(const std::string& tag) { m_tag = tag; }
        const std::string& GetTag() const { return m_tag; }

        // --- Parent-child hierarchy ---

        void SetParent(GameObject* parent);
        GameObject* GetParent() const { return m_parent; }
        const List<GameObject*>& GetChildren() const { return m_children; }
        void AddChild(GameObject* child);
        void RemoveChild(GameObject* child);
        bool HasParent() const { return m_parent != nullptr; }
        bool HasChildren() const { return m_children.GetSize() > 0; }

        // --- Type queries ---

        virtual bool IsLight() const { return false; }

        // --- Deferred destruction ---

        void MarkForDestroy() { m_pendingDestroy = true; }
        bool IsPendingDestroy() const { return m_pendingDestroy; }

        // --- Factory methods ---

        static GameObject* CreatePlane(Math::Vector3D position, int width = 100, int height = 100);
        static GameObject* CreateCube(Math::Vector3D position);
        static GameObject* CreateSphere(Math::Vector3D position, int stack = 16, int slices = 16);
        static GameObject* CreateCapsule(Math::Vector3D position, int segments = 16, int rings = 8, float height = 1, float radius = 0.5);
        static GameObject* CreateCylinder(Math::Vector3D position, int segments = 16, float height = 1, float radius = 0.5);
        static GameObject* CreateTorus(Math::Vector3D position, int segments = 16, int rings = 8, float innerRadius = 8, float outerRadius = 9);

    protected:
        bool bIsInitialized;

    private:
        bool m_isActive;
        bool m_pendingDestroy;
        std::string m_tag = "Untagged";

        List<RefPtr<Component>> Components;

        // Hierarchy
        GameObject* m_parent;
        List<GameObject*> m_children;

        void DestroyComponents();
    };
}

#endif // _GAMEOBJECT_H_
