#include "../../include/public/Core/SceneBase.hpp"
#include <Core/GameObject.hpp>
#include <Core/Logger.hpp>
#include <Camera/Camera.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Lighting/Light.hpp>
#include <Lighting/LightManager.hpp>
#include <Runtime/Skybox.hpp>
#include <Physics/PhysicsWorld.hpp>
#include <Physics/ColliderComponent.hpp>
#include <Debug/DebugLineRenderer.hpp>
#include "../../include/private/Graphics/Common/RenderCommandQueue.hpp"

namespace Sleak {

/// Calls fn on obj and the descendants it owns, skipping scene owned ones.
template <typename Fn>
static void ForEachOwned(GameObject* obj, Fn& fn) {
    if (!obj) return;
    fn(obj);
    const auto& children = obj->GetChildren();
    for (size_t i = 0; i < children.GetSize(); ++i) {
        if (children[i] && !children[i]->IsOwnedByScene())
            ForEachOwned(children[i], fn);
    }
}

/// Depth-first search of obj and the descendants it owns.
template <typename Pred>
static GameObject* FindOwned(GameObject* obj, const Pred& pred) {
    if (!obj) return nullptr;
    if (pred(obj)) return obj;
    const auto& children = obj->GetChildren();
    for (size_t i = 0; i < children.GetSize(); ++i) {
        if (!children[i] || children[i]->IsOwnedByScene()) continue;
        if (GameObject* found = FindOwned(children[i], pred)) return found;
    }
    return nullptr;
}

/// Registers lights and colliders in obj's owned subtree, skipping null
/// services.
static void RegisterTree(GameObject* obj, LightManager* lights,
                         Physics::PhysicsWorld* world) {
    auto reg = [&](GameObject* o) {
        if (lights && o->IsLight())
            lights->RegisterLight(static_cast<Light*>(o));
        if (world) {
            if (auto* collider = o->GetComponent<ColliderComponent>())
                world->RegisterCollider(collider);
        }
    };
    ForEachOwned(obj, reg);
}

/// Unregisters lights and colliders in obj's owned subtree.
static void UnregisterTree(GameObject* obj, LightManager* lights,
                           Physics::PhysicsWorld* world) {
    auto unreg = [&](GameObject* o) {
        if (lights && o->IsLight())
            lights->UnregisterLight(static_cast<Light*>(o));
        if (world) {
            if (auto* collider = o->GetComponent<ColliderComponent>())
                world->UnregisterCollider(collider);
        }
    };
    ForEachOwned(obj, unreg);
}

SceneBase::~SceneBase() {
    if (state != SceneState::Unloaded) {
        SLEAK_ERROR(
            "Scene '{0}' destroyed without Unload(); "
            "OnDeactivate/OnUnload did not run.",
            name);
    }

    DestroyAllObjects();
    delete m_lightManager;
    m_lightManager = nullptr;
    delete m_physicsWorld;
    m_physicsWorld = nullptr;
    delete m_skybox;
    m_skybox = nullptr;
}

void SceneBase::Load() {
    if (state != SceneState::Unloaded) return;
    state = SceneState::Loading;
    OnLoad();
    state = SceneState::Paused;
}

void SceneBase::Unload() {
    if (state == SceneState::Unloaded || state == SceneState::Unloading) return;

    Deactivate();

    state = SceneState::Unloading;

    OnUnload();

    DestroyAllObjects();

    if (auto* q = RenderEngine::RenderCommandQueue::GetInstance())
        q->ClearAll();

    bInitialized = false;
    bHasBegun = false;
    state = SceneState::Unloaded;
}

void SceneBase::Activate() {
    if (state == SceneState::Active) return;

    if (!bInitialized) Initialize();

    state = SceneState::Active;
    bActive = true;

    OnActivate();

    if (!bHasBegun) {
        bHasBegun = true;
        Begin();
    }
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

bool SceneBase::Initialize() {
    if (bInitialized) return true;

    LightManager* newLights = nullptr;
    if (!m_lightManager) {
        m_lightManager = newLights = new LightManager();
        m_lightManager->Initialize();
    }

    Physics::PhysicsWorld* newWorld = nullptr;
    if (!m_physicsWorld) {
        m_physicsWorld = newWorld = new Physics::PhysicsWorld();
    }

    if (newLights || newWorld) {
        for (size_t i = 0; i < Objects.GetSize(); ++i) {
            RegisterTree(Objects[i], newLights, newWorld);
        }
    }

    if (m_skybox && !m_skybox->IsInitialized()) {
        m_skybox->Initialize();
    }

    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i] && !Objects[i]->IsInitialized())
            Objects[i]->Initialize();
    }

    bInitialized = true;
    return true;
}

void SceneBase::Begin() {
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i]) Objects[i]->SetActive(true);
    }
}

void SceneBase::Update(float deltaTime) {
    if (!bActive) return;

    // Update objects FIRST so Camera::View/Projection reflect this frame's
    // rotation/position before any render-side UBO computes InvViewProj.
    // Why: LightManager::UpdateDeferredCB reads the static Camera matrices
    //      to build InvViewProj for the deferred lighting pass. If it ran
    //      before the camera object updated, InvViewProj was one frame stale
    //      and the lighting pass reconstructed wrong world positions on any
    //      camera rotation, causing whole-terrain shadow flicker.
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (Objects[i] && Objects[i]->IsActive() && !Objects[i]->HasParent()) {
            Objects[i]->Update(deltaTime);
        }
    }

    if (m_lightManager)
        m_lightManager->UpdateAndBind();

    if (m_skybox)
        m_skybox->Render();

    if (m_physicsWorld)
        m_physicsWorld->Step(deltaTime);

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

        auto drawObject = [&](GameObject* obj) {
            auto* collider = obj->GetComponent<ColliderComponent>();
            if (!collider) return;

            Math::Vector3D pos(0, 0, 0), scale(1, 1, 1);
            auto* transform = obj->GetComponent<TransformComponent>();
            if (transform) {
                pos = transform->GetWorldPosition() + collider->GetOffset();
                scale = transform->GetWorldScale();
            } else if (auto* cam = dynamic_cast<Camera*>(obj)) {
                pos = cam->GetPosition() + collider->GetOffset();
            }
            drawColliderShape(collider, pos, scale);
        };

        for (size_t i = 0; i < Objects.GetSize(); ++i) {
            ForEachOwned(Objects[i], drawObject);
        }
    }

    DebugLineRenderer::Flush(m_activeCamera);

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

void SceneBase::AddObject(GameObject* object) {
    if (!object) return;
    if (Objects.indexOf(object) != -1) return; // already in scene

    Objects.add(object);
    object->m_ownedByScene = true;

    RegisterTree(object, m_lightManager, m_physicsWorld);

    if (bInitialized && !object->IsInitialized()) {
        object->Initialize();
    }
}

void SceneBase::RemoveObject(GameObject* object) {
    if (!object) return;
    if (Objects.indexOf(object) == -1) return;

    List<GameObject*> children = object->GetChildren();
    for (size_t i = 0; i < children.GetSize(); ++i) {
        if (children[i] && children[i]->IsOwnedByScene())
            RemoveObject(children[i]);
    }

    UnregisterTree(object, m_lightManager, m_physicsWorld);

    auto dropPending = [&](GameObject* o) {
        int pending = m_pendingDestroy.indexOf(o);
        if (pending != -1) m_pendingDestroy.erase(pending);
    };
    ForEachOwned(object, dropPending);

    Objects.erase(Objects.indexOf(object));
    delete object;
}

void SceneBase::DestroyObject(GameObject* object) {
    if (!object) return;
    if (object->IsPendingDestroy()) return;

    object->MarkForDestroy();
    m_pendingDestroy.add(object);

    const auto& children = object->GetChildren();
    for (size_t i = 0; i < children.GetSize(); ++i) {
        if (children[i] && !children[i]->IsPendingDestroy()) {
            DestroyObject(children[i]);
        }
    }
}

GameObject* SceneBase::FindObjectByName(const std::string& objectName) {
    auto match = [&](GameObject* o) { return o->GetName() == objectName; };
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (GameObject* found = FindOwned(Objects[i], match)) return found;
    }
    return nullptr;
}

GameObject* SceneBase::FindObjectByID(uint64_t id) {
    auto match = [&](GameObject* o) { return o->GetUniqueID() == id; };
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        if (GameObject* found = FindOwned(Objects[i], match)) return found;
    }
    return nullptr;
}

List<GameObject*> SceneBase::FindObjectsByTag(const std::string& tag) {
    List<GameObject*> result;
    auto collect = [&](GameObject* o) {
        if (o->GetTag() == tag) result.add(o);
    };
    for (size_t i = 0; i < Objects.GetSize(); ++i) {
        ForEachOwned(Objects[i], collect);
    }
    return result;
}

void SceneBase::ProcessPendingDestroy() {
    if (m_pendingDestroy.empty()) return;

    List<GameObject*> pending = std::move(m_pendingDestroy);

    for (size_t i = 0; i < pending.GetSize(); ++i) {
        GameObject* obj = pending[i];
        UnregisterTree(obj, m_lightManager, m_physicsWorld);
        int index = Objects.indexOf(obj);
        if (index != -1) Objects.erase(index);
    }

    // Detach first so no pending object is deleted through its parent.
    for (size_t i = 0; i < pending.GetSize(); ++i) {
        if (pending[i]) pending[i]->SetParent(nullptr);
    }
    for (size_t i = 0; i < pending.GetSize(); ++i) {
        delete pending[i];
    }
}

void SceneBase::DestroyAllObjects() {
    m_pendingDestroy.clear();

    List<GameObject*> objects = std::move(Objects);

    for (size_t i = 0; i < objects.GetSize(); ++i) {
        UnregisterTree(objects[i], m_lightManager, m_physicsWorld);
    }
    for (size_t i = 0; i < objects.GetSize(); ++i) {
        if (objects[i]) objects[i]->SetParent(nullptr);
    }
    for (size_t i = 0; i < objects.GetSize(); ++i) {
        delete objects[i];
    }
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

} // namespace Sleak
