#include "../../include/public/Core/SceneBase.hpp"
#include <Core/GameObject.hpp>
#include <Logger.hpp>
#include <Camera/Camera.hpp>
#include <ECS/Components/FreeLookCameraController.hpp>
#include <Lighting/Light.hpp>
#include <Lighting/LightManager.hpp>
#include <Runtime/Skybox.hpp>

namespace Sleak {

// --- Destructor ---

SceneBase::~SceneBase() {
    DestroyAllObjects();
    delete m_lightManager;
    m_lightManager = nullptr;
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
        DebugCamera->Initialize();
        DebugCamera->GetComponent<FreeLookCameraController>()->SetEnabled(true);
        DebugCamera->SetActive(true);
        DebugCamera->SetLookTarget({0, 0, 0});
    }
}

} // namespace Sleak
