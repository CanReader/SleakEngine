#ifndef _GAMEOBJECT_H_
#define _GAMEOBJECT_H_

#include "Object.hpp"
#include <Core/Logger.hpp>
#include <ECS/Component.hpp>
#include <Utility/Container/List.hpp>
#include <Memory/RefPtr.hpp>
#include <type_traits>
#include <string>

namespace Sleak {
    namespace Math {class Vector3D;};
    /// Scene entity holding components and an optional parent/child transform
    /// hierarchy. Scene owns the instance; destroy via Scene::DestroyObject().
    class ENGINE_API GameObject : public Object {
    public:
        GameObject(const std::string& name = "GameObject")
            : Object(name), m_isActive(true), bIsInitialized(false),
              m_pendingDestroy(false), m_parent(nullptr) {}

        ~GameObject() override;

        /// Constructs and attaches a component of type T. Warns and no-ops if one already exists.
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

        /// Destroys and detaches the first component of type T, if present.
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

        /// Finds the first attached component of type T, or nullptr.
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

        /// True if a component of type T is attached.
        template <typename T>
        bool HasComponent() {
            static_assert(std::is_base_of_v<Component, T>,
                          "T must derive from Component!");
            return GetComponent<T>() != nullptr;
        }

        /// Initializes the object and its components; called once before the first Update.
        virtual void Initialize();
        virtual void Update(float deltaTime);
        virtual void FixedUpdate(float fixedDeltaTime);
        virtual void LateUpdate(float deltaTime);

        /// Enables or disables the object, firing OnEnable/OnDisable on its components.
        void SetActive(bool active);
        bool IsActive() const { return m_isActive; }

        void SetTag(const std::string& tag) { m_tag = tag; }
        const std::string& GetTag() const { return m_tag; }

        /// Reparents this object, updating both the old and new parent's child lists.
        void SetParent(GameObject* parent);
        GameObject* GetParent() const { return m_parent; }
        const List<GameObject*>& GetChildren() const { return m_children; }
        void AddChild(GameObject* child);
        void RemoveChild(GameObject* child);
        bool HasParent() const { return m_parent != nullptr; }
        bool HasChildren() const { return m_children.GetSize() > 0; }

        virtual bool IsLight() const { return false; }

        /// Flags the object for deferred destruction on the next scene pass.
        void MarkForDestroy() { m_pendingDestroy = true; }
        bool IsPendingDestroy() const { return m_pendingDestroy; }

        /// Built-in primitive factories, mainly for prototyping and debug scenes.
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

        /// Calls OnDestroy() on and drops every attached component.
        void DestroyComponents();
    };
}

#endif // _GAMEOBJECT_H_
