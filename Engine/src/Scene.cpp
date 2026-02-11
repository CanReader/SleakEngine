#include "../../include/public/Core/Scene.hpp"
#include <Logger.hpp>

namespace Sleak {

void Scene::OnLoad() {
    SLEAK_LOG("Scene '{0}' loaded.", GetName());
}

void Scene::OnUnload() {
    SLEAK_LOG("Scene '{0}' unloaded.", GetName());
}

void Scene::OnActivate() {
    SLEAK_LOG("Scene '{0}' activated.", GetName());
}

void Scene::OnDeactivate() {
    SLEAK_LOG("Scene '{0}' deactivated.", GetName());
}

bool Scene::Initialize() {
    return SceneBase::Initialize();
}

void Scene::Begin() {
    SceneBase::Begin();
}

void Scene::Update(float deltaTime) {
    SceneBase::Update(deltaTime);
}

void Scene::FixedUpdate(float fixedDeltaTime) {
    if (!bEnableFixedUpdate) return;
    SceneBase::FixedUpdate(fixedDeltaTime);
}

void Scene::LateUpdate(float deltaTime) {
    SceneBase::LateUpdate(deltaTime);
}

} // namespace Sleak
