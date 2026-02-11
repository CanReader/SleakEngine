#include <Core/GameObject.hpp>
#include <Runtime/InternalGeometry.hpp>
#include <Math/Vector.hpp>
#include <ECS/Components/MeshComponent.hpp>
#include <ECS/Components/TransformComponent.hpp>

namespace Sleak {

    // --- Destructor ---

    GameObject::~GameObject() {
        DestroyComponents();

        // Detach from parent
        if (m_parent) {
            m_parent->RemoveChild(this);
            m_parent = nullptr;
        }

        // Detach children (don't delete — scene owns them)
        for (size_t i = 0; i < m_children.GetSize(); ++i) {
            if (m_children[i]) {
                m_children[i]->m_parent = nullptr;
            }
        }
        m_children.clear();
    }

    // --- Lifecycle ---

    void GameObject::Initialize() {
        for (size_t i = 0; i < Components.GetSize(); ++i) {
            if (Components[i])
                Components[i]->Initialize();
        }
        bIsInitialized = true;
    }

    void GameObject::Update(float deltaTime) {
        if (!m_isActive || m_pendingDestroy) return;

        for (size_t i = 0; i < Components.GetSize(); ++i) {
            if (Components[i])
                Components[i]->Update(deltaTime);
        }

        // Recursively update children
        for (size_t i = 0; i < m_children.GetSize(); ++i) {
            if (m_children[i] && m_children[i]->IsActive())
                m_children[i]->Update(deltaTime);
        }
    }

    void GameObject::FixedUpdate(float fixedDeltaTime) {
        if (!m_isActive || m_pendingDestroy) return;

        for (size_t i = 0; i < Components.GetSize(); ++i) {
            if (Components[i])
                Components[i]->FixedUpdate(fixedDeltaTime);
        }

        for (size_t i = 0; i < m_children.GetSize(); ++i) {
            if (m_children[i] && m_children[i]->IsActive())
                m_children[i]->FixedUpdate(fixedDeltaTime);
        }
    }

    void GameObject::LateUpdate(float deltaTime) {
        if (!m_isActive || m_pendingDestroy) return;

        for (size_t i = 0; i < Components.GetSize(); ++i) {
            if (Components[i])
                Components[i]->LateUpdate(deltaTime);
        }

        for (size_t i = 0; i < m_children.GetSize(); ++i) {
            if (m_children[i] && m_children[i]->IsActive())
                m_children[i]->LateUpdate(deltaTime);
        }
    }

    void GameObject::SetActive(bool active) {
        if (m_isActive == active) return;
        m_isActive = active;

        // Notify components
        for (size_t i = 0; i < Components.GetSize(); ++i) {
            if (Components[i]) {
                if (active)
                    Components[i]->OnEnable();
                else
                    Components[i]->OnDisable();
            }
        }

        // Propagate to children
        for (size_t i = 0; i < m_children.GetSize(); ++i) {
            if (m_children[i])
                m_children[i]->SetActive(active);
        }
    }

    // --- Parent-child hierarchy ---

    void GameObject::SetParent(GameObject* parent) {
        if (m_parent == parent) return;

        // Remove from old parent's children list
        if (m_parent) {
            m_parent->RemoveChild(this);
        }

        m_parent = parent;

        // Add to new parent's children list
        if (m_parent) {
            // Avoid duplicate adds
            if (m_parent->m_children.indexOf(this) == -1) {
                m_parent->m_children.add(this);
            }
        }
    }

    void GameObject::AddChild(GameObject* child) {
        if (!child || child == this) return;
        child->SetParent(this);
    }

    void GameObject::RemoveChild(GameObject* child) {
        if (!child) return;
        int index = m_children.indexOf(child);
        if (index != -1) {
            m_children.erase(index);
            child->m_parent = nullptr;
        }
    }

    // --- Internal ---

    void GameObject::DestroyComponents() {
        for (size_t i = 0; i < Components.GetSize(); ++i) {
            if (Components[i])
                Components[i]->OnDestroy();
        }
        Components.clear();
    }

    // --- Factory methods ---

    GameObject* GameObject::CreatePlane(Math::Vector3D position, int width, int height) {
        GameObject* object = new GameObject("Plane");

        object->AddComponent<MeshComponent>(GetPlaneMesh(width, height));
        object->AddComponent<TransformComponent>(position);
        object->Initialize();

        return object;
    }

    GameObject* GameObject::CreateCube(Math::Vector3D position) {
        GameObject* object = new GameObject("Cube");

        object->AddComponent<MeshComponent>(GetCubeMesh());
        object->AddComponent<TransformComponent>(position);
        object->Initialize();

        return object;
    }

    GameObject* GameObject::CreateSphere(Math::Vector3D position, int stack, int slices) {
        GameObject* object = new GameObject("Sphere");

        object->AddComponent<MeshComponent>(GetSphereMesh(stack, slices));
        object->AddComponent<TransformComponent>(position);
        object->Initialize();

        return object;
    }

    GameObject* GameObject::CreateCapsule(Math::Vector3D position, int segments, int rings, float height, float radius) {
        GameObject* object = new GameObject("Capsule");

        object->AddComponent<MeshComponent>(GetCapsuleMesh(segments, rings, height, radius));
        object->AddComponent<TransformComponent>(position);
        object->Initialize();

        return object;
    }

    GameObject* GameObject::CreateCylinder(Math::Vector3D position, int segments, float height, float radius) {
        GameObject* object = new GameObject("Cylinder");

        object->AddComponent<MeshComponent>(GetCylinderMesh(segments, height, radius));
        object->AddComponent<TransformComponent>(position);
        object->Initialize();

        return object;
    }
}
