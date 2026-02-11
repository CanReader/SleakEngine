#include <Lighting/LightManager.hpp>
#include <Lighting/Light.hpp>
#include <Graphics/ConstantBuffer.hpp>
#include <Graphics/ResourceManager.hpp>
#include <Graphics/BufferBase.hpp>
#include <Camera/Camera.hpp>
#include <Logger.hpp>

namespace Sleak {

LightManager::LightManager() = default;

LightManager::~LightManager() = default;

void LightManager::Initialize() {
    if (!m_lightBuffer) {
        m_lightBuffer = RefPtr<RenderEngine::BufferBase>(
            RenderEngine::ResourceManager::CreateBuffer(
                RenderEngine::BufferType::Constant,
                sizeof(RenderEngine::LightCBData),
                nullptr));
        m_lightBuffer->SetSlot(2);
    }
}

void LightManager::RegisterLight(Light* light) {
    if (!light) return;
    if (m_lights.indexOf(light) != -1) return;

    if (m_lights.GetSize() >= RenderEngine::MAX_LIGHTS) {
        SLEAK_WARN(
            "Maximum light count ({}) reached, cannot register "
            "light '{}'",
            RenderEngine::MAX_LIGHTS, light->GetName());
        return;
    }

    m_lights.add(light);
}

void LightManager::UnregisterLight(Light* light) {
    if (!light) return;
    int index = m_lights.indexOf(light);
    if (index != -1) {
        m_lights.erase(index);
    }
}

void LightManager::UpdateAndBind() {
    if (!m_lightBuffer) return;

    RenderEngine::LightCBData cbData{};

    // Camera position
    const auto& camPos = Camera::GetMainCameraPosition();
    cbData.CameraPosX = camPos.GetX();
    cbData.CameraPosY = camPos.GetY();
    cbData.CameraPosZ = camPos.GetZ();

    // Ambient
    cbData.AmbientR = m_ambientR;
    cbData.AmbientG = m_ambientG;
    cbData.AmbientB = m_ambientB;
    cbData.AmbientIntensity = m_ambientIntensity;

    // Collect active lights
    uint32_t count = 0;
    for (size_t i = 0;
         i < m_lights.GetSize() && count < RenderEngine::MAX_LIGHTS;
         ++i) {
        Light* light = m_lights[i];
        if (!light || !light->IsEnabled()) continue;

        cbData.Lights[count] = light->BuildGPUData();
        ++count;
    }
    cbData.NumActiveLights = count;

    // Update and bind at slot 2
    m_lightBuffer->Update(&cbData, sizeof(cbData));
    m_lightBuffer->Update();
}

void LightManager::SetAmbientColor(float r, float g, float b) {
    m_ambientR = r;
    m_ambientG = g;
    m_ambientB = b;
}

}  // namespace Sleak
