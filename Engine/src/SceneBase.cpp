#include "../../include/public/Core/SceneBase.hpp"
#include <Core/GameObject.hpp>
#include <Logger.hpp>
#include <Camera/Camera.hpp>
#include <ECS/Components/FreeLookCameraController.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Lighting/Light.hpp>
#include <Lighting/LightManager.hpp>
#include <Runtime/Skybox.hpp>
#include <Physics/PhysicsWorld.hpp>
#include <Physics/ColliderComponent.hpp>
#include <Physics/RigidbodyComponent.hpp>
#include <Debug/DebugLineRenderer.hpp>

namespace Sleak {

// --- Physics helpers (needed early for Initialize) ---

static void RegisterCollidersRecursive(GameObject* obj, Physics::PhysicsWorld* world) {
    if (!obj || !world) return;
    auto* collider = obj->GetComponent<ColliderComponent>();
    if (collider) {
        world->RegisterCollider(collider);
    }
    for (size_t i = 0; i < obj->GetChildren().GetSize(); ++i) {
        RegisterCollidersRecursive(obj->GetChildren()[i], world);
    }
}

static void UnregisterCollidersRecursive(GameObject* obj, Physics::PhysicsWorld* world) {
    if (!obj || !world) return;
    auto* collider = obj->GetComponent<ColliderComponent>();
    if (collider) {
        world->UnregisterCollider(collider);
    }
    for (size_t i = 0; i < obj->GetChildren().GetSize(); ++i) {
        UnregisterCollidersRecursive(obj->GetChildren()[i], world);
    }
}

// --- Destructor ---

SceneBase::~SceneBase() {
    DestroyAllObjects();
    delete m_lightManager;
    m_lightManager = nullptr;
    delete m_physicsWorld;
    m_physicsWorld = nullptr;
    delete m_skybox;
    m_skybox = nullptr;
}

// --- State transitions ---

void SceneBase::Load() {
    if (state != SceneState::Unloaded) return;
    state = SceneState::Loading;
    OnLoad();
    state = SceneState::Paused;
}

void SceneBase::Unload() {
    if (state == SceneState::Unloaded || state == SceneState::Unloading) return;
    state = SceneState::Unloading;

    if (bActive) Deactivate();

    OnUnload();

    DestroyAllObjects();
    bInitialized = false;
    state = SceneState::Unloaded;
}

void SceneBase::Activate() {
    if (state == SceneState::Active) return;

    if (!bInitialized) Initialize();

    state = SceneState::Active;
    bActive = true;

    OnActivate();

    Begin();
}

void SceneBase::Deactivate() {
    if (state != SceneState::Active) return;
    OnDeactivate();
    bActive = false;
    state = SceneState::Paused;
}

void SceneBase::Pause() {
    if (state != SceneState::Active) return;
    state = SceneState::Paused;
    bActive = false;
}

void SceneBase::Resume() {
    if (state != SceneState::Paused) return;
    state = SceneState::Active;
    bActive = true;
}

// --- Initialization ---

bool SceneBase::Initialize() {
    if (bInitialized) return true;

    if (!m_lightManager) {
        m_lightManager = new LightManager();
        m_lightManager->Initialize();
    }

    if (!m_physicsWorld) {
        m_physicsWorld = new Physics::PhysicsWorld();

        // Register colliders from objects added before PhysicsWorld existed
        for (size_t i = 0; i < Objects.GetSize(); ++i) {
            if (Objects[i]) {
                RegisterCollidersRecursive(Objects[i], m_physicsWorld);
            }
        }
    }

    InitializeDebugCamera();

    if (m_skybox && !m_skybox->IsInitialized()) {
        m_skybox->Initialize();
    }

    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i]) Objects[i]->Initialize();
    }

    bInitialized = true;
    return true;
}

void SceneBase::Begin() {
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i]) Objects[i]->SetActive(true);
    }
}

// --- Update loops ---

void SceneBase::Update(float deltaTime) {
    if (!bActive) return;

    // Update lighting constant buffer before rendering
    if (m_lightManager)
        m_lightManager->UpdateAndBind();

    // Only update root objects — children update recursively
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i] && Objects[i]->IsActive() && !Objects[i]->HasParent()) {
            Objects[i]->Update(deltaTime);
        }
    }

    // Render skybox after scene objects (depth testing ensures it appears behind)
    if (m_skybox)
        m_skybox->Render();

    if (DebugCamera)
        DebugCamera->Update(deltaTime);

    // Resolve collisions after all movement is done
    if (m_physicsWorld)
        m_physicsWorld->Step(deltaTime);

    // Debug line rendering for colliders
    if (DebugLineRenderer::IsEnabled()) {
        auto drawColliderShape = [](ColliderComponent* collider, const Math::Vector3D& worldPos, const Math::Vector3D& worldScale) {
            const auto& shape = collider->GetShape();

            if (auto* aabb = std::get_if<Physics::AABB>(&shape)) {
                Physics::AABB worldAABB(
                    aabb->min * worldScale + worldPos,
                    aabb->max * worldScale + worldPos);
                DebugLineRenderer::DrawAABB(worldAABB, 0.0f, 1.0f, 0.0f);
            } else if (auto* sphere = std::get_if<Physics::BoundingSphere>(&shape)) {
                Math::Vector3D center = sphere->center * worldScale + worldPos;
                float maxScale = std::max({worldScale.GetX(), worldScale.GetY(), worldScale.GetZ()});
                DebugLineRenderer::DrawSphere(center, sphere->radius * maxScale, 0.0f, 1.0f, 0.0f);
            } else if (auto* capsule = std::get_if<Physics::BoundingCapsule>(&shape)) {
                Physics::BoundingCapsule worldCapsule = *capsule;
                worldCapsule.center = capsule->center * worldScale + worldPos;
                float maxScale = std::max({worldScale.GetX(), worldScale.GetY(), worldScale.GetZ()});
                worldCapsule.radius = capsule->radius * maxScale;
                worldCapsule.halfHeight = capsule->halfHeight * maxScale;
                DebugLineRenderer::DrawCapsule(worldCapsule, 0.0f, 1.0f, 0.0f);
            }
        };

        for (size_t i = 0; i < Objects.GetSize(); ++i) {
            if (!Objects[i]) continue;
            auto* collider = Objects[i]->GetComponent<ColliderComponent>();
            if (!collider) continue;

            Math::Vector3D pos(0, 0, 0), scale(1, 1, 1);
            auto* transform = Objects[i]->GetComponent<TransformComponent>();
            if (transform) {
                pos = transform->GetWorldPosition() + collider->GetOffset();
                scale = transform->GetWorldScale();
            } else if (auto* cam = dynamic_cast<Camera*>(Objects[i])) {
                pos = cam->GetPosition() + collider->GetOffset();
            }
            drawColliderShape(collider, pos, scale);
        }

        // Also draw debug camera collider
        if (DebugCamera.IsValid()) {
            auto* collider = DebugCamera->GetComponent<ColliderComponent>();
            if (collider) {
                Math::Vector3D pos = DebugCamera->GetPosition() + collider->GetOffset();
                drawColliderShape(collider, pos, Math::Vector3D(1, 1, 1));
            }
        }

        DebugLineRenderer::Flush(DebugCamera.IsValid() ? DebugCamera.get() : nullptr);
    }

    // Process deferred destruction at end of frame
    ProcessPendingDestroy();
}

void SceneBase::FixedUpdate(float fixedDeltaTime) {
    if (!bActive) return;

    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i] && Objects[i]->IsActive() && !Objects[i]->HasParent()) {
            Objects[i]->FixedUpdate(fixedDeltaTime);
        }
    }
}

void SceneBase::LateUpdate(float deltaTime) {
    if (!bActive) return;

    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i] && Objects[i]->IsActive() && !Objects[i]->HasParent()) {
            Objects[i]->LateUpdate(deltaTime);
        }
    }
}

// --- Object management ---

void SceneBase::AddObject(GameObject* object) {
    if (!object) return;
    if (Objects.indexOf(object) != -1) return; // already in scene

    Objects.add(object);

    // Auto-register lights with the LightManager
    if (m_lightManager && object->IsLight()) {
        m_lightManager->RegisterLight(
            static_cast<Light*>(object));
    }

    // Auto-register colliders with the PhysicsWorld
    if (m_physicsWorld) {
        RegisterCollidersRecursive(object, m_physicsWorld);
    }

    // If scene is already initialized, initialize the new object
    if (bInitialized && !object->IsActive()) {
        object->Initialize();
        if (bActive) object->SetActive(true);
    }
}

void SceneBase::RemoveObject(GameObject* object) {
    if (!object) return;
    int index = Objects.indexOf(object);
    if (index != -1) {
        if (m_lightManager && object->IsLight()) {
            m_lightManager->UnregisterLight(
                static_cast<Light*>(object));
        }
        if (m_physicsWorld) {
            UnregisterCollidersRecursive(object, m_physicsWorld);
        }
        Objects.erase(index);
        delete object;
    }
}

void SceneBase::DestroyObject(GameObject* object) {
    if (!object) return;
    if (object->IsPendingDestroy()) return;

    object->MarkForDestroy();
    m_pendingDestroy.add(object);

    // Mark children for destruction too
    const auto& children = object->GetChildren();
    for (size_t i = 0; i < children.GetSize(); ++i) {
        if (children[i] && !children[i]->IsPendingDestroy()) {
            DestroyObject(children[i]);
        }
    }
}

// --- Object queries ---

GameObject* SceneBase::FindObjectByName(const std::string& objectName) {
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i] && Objects[i]->GetName() == objectName) {
            return Objects[i];
        }
    }
    return nullptr;
}

GameObject* SceneBase::FindObjectByID(uint64_t id) {
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i] && Objects[i]->GetUniqueID() == id) {
            return Objects[i];
        }
    }
    return nullptr;
}

List<GameObject*> SceneBase::FindObjectsByTag(const std::string& tag) {
    List<GameObject*> result;
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i] && Objects[i]->GetTag() == tag) {
            result.add(Objects[i]);
        }
    }
    return result;
}

// --- Internal ---

void SceneBase::ProcessPendingDestroy() {
    if (m_pendingDestroy.empty()) return;

    for (size_t i = 0; i < m_pendingDestroy.GetSize(); ++i) {
        GameObject* obj = m_pendingDestroy[i];
        if (!obj) continue;

        // Unregister lights
        if (m_lightManager && obj->IsLight()) {
            m_lightManager->UnregisterLight(
                static_cast<Light*>(obj));
        }

        // Unregister colliders
        if (m_physicsWorld) {
            UnregisterCollidersRecursive(obj, m_physicsWorld);
        }

        // Remove from the main objects list
        int index = Objects.indexOf(obj);
        if (index != -1) {
            Objects.erase(index);
        }

        delete obj;
    }
    m_pendingDestroy.clear();
}

void SceneBase::DestroyAllObjects() {
    // Clear pending list first (those are also in Objects)
    m_pendingDestroy.clear();

    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        delete Objects[i];
    }
    Objects.clear();
}

void SceneBase::SetSkybox(Skybox* skybox) {
    if (m_skybox) {
        delete m_skybox;
    }
    m_skybox = skybox;
    if (m_skybox && bInitialized && !m_skybox->IsInitialized()) {
        m_skybox->Initialize();
    }
}

void SceneBase::InitializeDebugCamera() {
    if (!DebugCamera.IsValid()) {
        DebugCamera = ObjectPtr<Camera>(new Camera(
            std::string("SceneDebugCamera"),
            {2, 3, -6}, 60, 0.01, 100));
        DebugCamera->AddComponent<FreeLookCameraController>();
        DebugCamera->AddComponent<ColliderComponent>(
            Physics::BoundingSphere(Math::Vector3D(0, 0, 0), 0.3f));
        DebugCamera->AddComponent<RigidbodyComponent>(BodyType::Kinematic);
        DebugCamera->Initialize();
        DebugCamera->GetComponent<FreeLookCameraController>()->SetEnabled(true);

        // Register camera collider with physics
        if (m_physicsWorld) {
            auto* collider = DebugCamera->GetComponent<ColliderComponent>();
            if (collider) {
                m_physicsWorld->RegisterCollider(collider);
            }
        }

        DebugCamera->SetActive(true);
        DebugCamera->SetLookTarget({0, 0, 0});
    }
}

} // namespace Sleak
