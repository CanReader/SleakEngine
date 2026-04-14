#include <Lighting/LightManager.hpp>
#include <Lighting/Light.hpp>
#include <Lighting/DirectionalLight.hpp>
#include <Graphics/ConstantBuffer.hpp>
#include <Graphics/ResourceManager.hpp>
#include <Graphics/BufferBase.hpp>
#include <Graphics/Renderer.hpp>
#include <Graphics/RenderContext.hpp>
#include <Camera/Camera.hpp>
#include <Window.hpp>
#include <Core/Application.hpp>
#include <Math/Matrix.hpp>
#include <Core/Timer.hpp>
#include <Logger.hpp>
#include <cstring>
#include <cmath>

namespace {
// 4x4 row-major matrix inverse via cofactors (Cramer's rule).
// Returns false if matrix is singular.
static bool Invert4x4(const float m[16], float inv[16]) {
    float inv0  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    float inv4  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    float inv8  =  m[4]*m[9] *m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    float inv12 = -m[4]*m[9] *m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

    float det = m[0]*inv0 + m[1]*inv4 + m[2]*inv8 + m[3]*inv12;
    if (std::fabsf(det) < 1e-8f) return false;
    float id = 1.0f / det;

    inv[0]  = inv0  * id;
    inv[4]  = inv4  * id;
    inv[8]  = inv8  * id;
    inv[12] = inv12 * id;

    inv[1]  = (-m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10]) * id;
    inv[5]  = ( m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10]) * id;
    inv[9]  = (-m[0]*m[9] *m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9] ) * id;
    inv[13] = ( m[0]*m[9] *m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9] ) * id;

    inv[2]  = ( m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6]) * id;
    inv[6]  = (-m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6]) * id;
    inv[10] = ( m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5]) * id;
    inv[14] = (-m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5]) * id;

    inv[3]  = (-m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6]) * id;
    inv[7]  = ( m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6]) * id;
    inv[11] = (-m[0]*m[5]*m[11] + m[0]*m[7]*m[9]  + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7] + m[8]*m[3]*m[5]) * id;
    inv[15] = ( m[0]*m[5]*m[10] - m[0]*m[6]*m[9]  - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6] - m[8]*m[2]*m[5]) * id;

    return true;
}
} // namespace

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

    // Fog
    if (m_fogEnabled) {
        cbData.FogColorR = m_fogR;
        cbData.FogColorG = m_fogG;
        cbData.FogColorB = m_fogB;
        cbData.FogColorA = 1.0f;
        cbData.FogStart = m_fogStart;
        cbData.FogEnd = m_fogEnd;
    } else {
        cbData.FogStart = 0.0f;
        cbData.FogEnd = 0.0f;
    }

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

    // Update deferred CB (InvViewProj + screen size) for the lighting pass
    UpdateDeferredCB();
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

    // Tell the renderer whether the shadow pass should run
    renderer->SetShadowPassEnabled(shadowLight != nullptr);

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

        // Light position: follow camera XZ but fix Y at world origin.
        // Anchoring Y prevents the shadow frustum from shifting vertically
        // when the player jumps/flies, which causes hard Z-plane cutoff flicker.
        const auto& camPos = Camera::GetMainCameraPosition();
        Math::Vector3D lightPos = Math::Vector3D(camPos.GetX(), 0.0f, camPos.GetZ())
                                + dir * (-shadowDist);

        // Convert to Vector<float,3> for Matrix methods
        Math::Vector<float, 3> lp({lightPos.GetX(), lightPos.GetY(), lightPos.GetZ()});
        Math::Vector<float, 3> ld({dir.GetX(), dir.GetY(), dir.GetZ()});

        // Avoid degenerate LookTo when light direction is nearly vertical
        // (cross product with (0,1,0) would be zero → NaN matrix)
        Math::Vector<float, 3> up = (fabsf(dir.GetY()) > 0.999f)
            ? Math::Vector<float, 3>({0.0f, 0.0f, 1.0f})
            : Math::Vector<float, 3>({0.0f, 1.0f, 0.0f});

        Math::Matrix4 lightView = Math::Matrix4::LookTo(lp, ld, up);

        // ---- Texel snapping: stabilize shadow map when camera moves ----
        // Snap the light-space origin to shadow map texel boundaries so
        // sub-texel camera movement doesn't shift the entire shadow map.
        constexpr float shadowMapSize = 2048.0f;
        float texelStep = (2.0f * frustumSize) / shadowMapSize;

        // Transform world origin through the light view to get the
        // current light-space offset, then snap X/Y to texel grid.
        // Row-major convention: point * Matrix → row 3 holds translation.
        float lsX = lightView(3, 0);
        float lsY = lightView(3, 1);
        lightView(3, 0) = std::floor(lsX / texelStep) * texelStep;
        lightView(3, 1) = std::floor(lsY / texelStep) * texelStep;

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

    // CameraPos.w carries a monotonic scene clock, consumed by shaders that
    // need animation time (e.g. water waves on the Vulkan backend — the engine
    // has no MaterialUBO slot in its Vulkan pipeline layout, so there is
    // nowhere else to stash time). Using a static Timer keeps this
    // self-contained and independent of Application state.
    static Sleak::Timer s_sceneClock;
    ubo.CameraPos[0] = camPos.GetX();
    ubo.CameraPos[1] = camPos.GetY();
    ubo.CameraPos[2] = camPos.GetZ();
    ubo.CameraPos[3] = s_sceneClock.Elapsed();

    // Copy light VP matrix
    std::memcpy(ubo.LightVP, &lightVP(0, 0), sizeof(float) * 16);

    ubo.ShadowBias = shadowLight ? shadowLight->GetShadowBias() : 0.0f;
    ubo.ShadowStrength = shadowLight ? shadowLight->GetShadowStrength() : 0.0f;
    ubo.ShadowTexelSize = 1.0f / 2048.0f;  // Match SHADOW_MAP_SIZE
    ubo.LightSize = shadowLight ? shadowLight->GetLightSize() : 0.0f;

    // Fog
    if (m_fogEnabled) {
        ubo.FogColor[0] = m_fogR;
        ubo.FogColor[1] = m_fogG;
        ubo.FogColor[2] = m_fogB;
        ubo.FogColor[3] = 1.0f;
        ubo.FogStart = m_fogStart;
        ubo.FogEnd = m_fogEnd;
    } else {
        ubo.FogStart = 0.0f;
        ubo.FogEnd = 0.0f;
    }

    renderer->UpdateShadowLightUBO(&ubo, sizeof(ubo));
}

void LightManager::SetAmbientColor(float r, float g, float b) {
    m_ambientR = r;
    m_ambientG = g;
    m_ambientB = b;
}

void LightManager::UpdateDeferredCB() {
    auto* app = Application::GetInstance();
    if (!app) return;
    auto* renderer = app->GetRenderer();
    if (!renderer) return;
    auto* ctx = renderer->GetContext();
    if (!ctx || !ctx->IsDeferredEnabled()) return;

    // ViewProj = View * Proj (row-major engine convention)
    const Math::Matrix4& V = Camera::GetMainViewMatrix();
    const Math::Matrix4& P = Camera::GetMainProjectionMatrix();
    Math::Matrix4 VP = V * P;

    RenderEngine::DeferredCBData cb{};
    if (!Invert4x4(&VP(0, 0), cb.InvViewProj)) {
        // Singular matrix — skip update (can happen during initialization)
        return;
    }

    // Screen size from Window static state
    cb.ScreenWidth  = static_cast<float>(app->GetWindow().GetWidth());
    cb.ScreenHeight = static_cast<float>(app->GetWindow().GetHeight());
    cb.NearPlane    = 0.1f;
    cb.FarPlane     = 2000.0f;

    ctx->UpdateDeferredCB(&cb, sizeof(cb));
}

}  // namespace Sleak
