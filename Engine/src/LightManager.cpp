#include <Lighting/LightManager.hpp>
#include <Lighting/Light.hpp>
#include <Lighting/DirectionalLight.hpp>
#include <Graphics/ConstantBuffer.hpp>
#include <Graphics/ResourceManager.hpp>
#include <Graphics/BufferBase.hpp>
#include <Graphics/Renderer.hpp>
#include <Camera/Camera.hpp>
#include <Core/Application.hpp>
#include <Math/Matrix.hpp>
#include <Logger.hpp>
#include <cstring>

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

    // Update shadow data for Vulkan renderer
    UpdateShadowData();
}

void LightManager::UpdateShadowData() {
    auto* app = Application::GetInstance();
    if (!app) return;
    auto* renderer = app->GetRenderer();
    if (!renderer) return;

    // Find first shadow-casting directional light
    DirectionalLight* shadowLight = nullptr;
    for (size_t i = 0; i < m_lights.GetSize(); ++i) {
        Light* light = m_lights[i];
        if (!light || !light->IsEnabled() || !light->GetCastShadows()) continue;

        auto* dirLight = dynamic_cast<DirectionalLight*>(light);
        if (dirLight) {
            shadowLight = dirLight;
            break;
        }
    }

    // Find first enabled directional light (regardless of shadow casting)
    // for populating light/ambient data in the UBO
    DirectionalLight* anyDirLight = nullptr;
    if (!shadowLight) {
        for (size_t i = 0; i < m_lights.GetSize(); ++i) {
            Light* light = m_lights[i];
            if (!light || !light->IsEnabled()) continue;
            auto* dirLight = dynamic_cast<DirectionalLight*>(light);
            if (dirLight) {
                anyDirLight = dirLight;
                break;
            }
        }
    }

    // Use shadow light if available, otherwise fall back to any directional light
    DirectionalLight* activeLight = shadowLight ? shadowLight : anyDirLight;

    if (!activeLight) {
        static bool warned = false;
        if (!warned) { SLEAK_WARN("UpdateShadowData: No directional light found!"); warned = true; }
        return;
    }

    auto dir = activeLight->GetDirection();
    auto color = activeLight->GetColor();
    float intensity = activeLight->GetIntensity();

    Math::Matrix4 lightVP = Math::Matrix4::Identity();

    if (shadowLight) {
        // Compute light view-projection matrix from shadow configuration
        float frustumSize = shadowLight->GetShadowFrustumSize();
        float shadowDist  = shadowLight->GetShadowDistance();
        float nearP       = shadowLight->GetShadowNearPlane();
        float farP        = shadowLight->GetShadowFarPlane();

        // Light position: offset from scene center along negative light direction
        Math::Vector3D lightPos = dir * (-shadowDist);

        // Convert to Vector<float,3> for Matrix methods
        Math::Vector<float, 3> lp({lightPos.GetX(), lightPos.GetY(), lightPos.GetZ()});
        Math::Vector<float, 3> ld({dir.GetX(), dir.GetY(), dir.GetZ()});
        Math::Vector<float, 3> up({0.0f, 1.0f, 0.0f});

        Math::Matrix4 lightView = Math::Matrix4::LookTo(lp, ld, up);

        // Build Vulkan-compatible orthographic projection (LH, [0,1] depth range)
        // Engine stores row-major, GLSL reads column-major (transposed) —
        // translations go in ROW 3 so they end up in GLSL column 3.
        float left = -frustumSize, right = frustumSize;
        float bottom = -frustumSize, top = frustumSize;
        Math::Matrix4 lightProj = Math::Matrix4::Identity();
        lightProj(0, 0) = 2.0f / (right - left);
        lightProj(1, 1) = 2.0f / (top - bottom);
        lightProj(2, 2) = 1.0f / (farP - nearP);
        lightProj(3, 0) = -(right + left) / (right - left);
        lightProj(3, 1) = -(top + bottom) / (top - bottom);
        lightProj(3, 2) = -nearP / (farP - nearP);

        // LightVP = View * Projection (row-major convention)
        lightVP = lightView * lightProj;
    }

    // Set the light VP matrix on the renderer
    renderer->SetLightVP(&lightVP(0, 0));

    // Build shadow light UBO
    const auto& camPos = Camera::GetMainCameraPosition();
    RenderEngine::ShadowLightUBO ubo{};

    ubo.LightDir[0] = dir.GetX();
    ubo.LightDir[1] = dir.GetY();
    ubo.LightDir[2] = dir.GetZ();
    ubo.LightDir[3] = shadowLight ? shadowLight->GetShadowNormalBias() : 0.0f;

    ubo.LightColor[0] = color.GetX();
    ubo.LightColor[1] = color.GetY();
    ubo.LightColor[2] = color.GetZ();
    ubo.LightColor[3] = intensity;

    ubo.Ambient[0] = m_ambientR;
    ubo.Ambient[1] = m_ambientG;
    ubo.Ambient[2] = m_ambientB;
    ubo.Ambient[3] = m_ambientIntensity;

    ubo.CameraPos[0] = camPos.GetX();
    ubo.CameraPos[1] = camPos.GetY();
    ubo.CameraPos[2] = camPos.GetZ();
    ubo.CameraPos[3] = 0.0f;

    // Copy light VP matrix
    std::memcpy(ubo.LightVP, &lightVP(0, 0), sizeof(float) * 16);

    ubo.ShadowBias = shadowLight ? shadowLight->GetShadowBias() : 0.0f;
    ubo.ShadowStrength = shadowLight ? shadowLight->GetShadowStrength() : 0.0f;
    ubo.ShadowTexelSize = 1.0f / 4096.0f;  // Match SHADOW_MAP_SIZE
    ubo.LightSize = shadowLight ? shadowLight->GetLightSize() : 0.0f;

    renderer->UpdateShadowLightUBO(&ubo, sizeof(ubo));
}

void LightManager::SetAmbientColor(float r, float g, float b) {
    m_ambientR = r;
    m_ambientG = g;
    m_ambientB = b;
}

}  // namespace Sleak
