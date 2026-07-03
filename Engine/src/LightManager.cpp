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

    // Fog — horizon color + sky-zenith blend + exponential height fog.
    // OpenGL deferred lighting reads its fog parameters from this LightCBData
    // (binding 2). Vulkan/forward shaders read the same data from
    // ShadowLightUBO. Keep both blocks in sync.
    if (m_fogEnabled) {
        cbData.FogColorR = m_fogR;
        cbData.FogColorG = m_fogG;
        cbData.FogColorB = m_fogB;
        cbData.FogColorA = 1.0f;
        cbData.FogStart = m_fogStart;
        cbData.FogEnd = m_fogEnd;

        cbData.FogColorZenith[0] = m_fogZenithR;
        cbData.FogColorZenith[1] = m_fogZenithG;
        cbData.FogColorZenith[2] = m_fogZenithB;
        cbData.FogColorZenith[3] = 1.0f;

        cbData.HeightFogTop     = m_heightFogTop;
        cbData.HeightFogDensity = m_heightFogDensity;
        cbData.HeightFogFalloff = m_heightFogFalloff;
        cbData.HeightFogEnabled = m_heightFogEnabled ? 1.0f : 0.0f;
    } else {
        cbData.FogStart = 0.0f;
        cbData.FogEnd = 0.0f;
        cbData.HeightFogEnabled = 0.0f;
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

        // ---- Texel snap (DirectX SDK standard technique) ----
        // Project world origin through the raw lightVP, measure its XY in
        // shadow-map texel space, snap to nearest texel, apply the delta
        // back to the projection matrix. This guarantees the sampling grid
        // is aligned to world-space texel cells so sub-texel camera motion
        // never shifts which texel a world point lands on → no shimmer.
        //
        // Using round() (not floor) — floor flips by a full texel when
        // the fractional part crosses 0 due to float noise.
        constexpr float shadowMapSize = 2048.0f;
        constexpr float halfShadow = shadowMapSize * 0.5f;

        Math::Matrix4 lightVP_raw = lightView * lightProj;
        // Project the CAMERA position through lightVP — NOT the world origin.
        // The world origin is a fixed point: snapping it produces zero offset
        // whenever it already lies on a texel boundary, so the snap becomes a
        // no-op and shadows still shimmer. The camera position drifts
        // fractionally through the light-texel grid as the player moves, and
        // snapping THAT point's projected location produces the corrective
        // offset that keeps every world texel locked to the same shadow texel.
        // Row-vector convention: (cx, cy, cz, 1) * M.
        // Y is forced to 0 — using the eye-height Y here lets every physics
        // sub-tick of the camera (gravity / MTV correction even while
        // "stationary" on the ground) feed sub-mm oscillations into the snap
        // input, which can flip round() between adjacent texels and produce a
        // visible 1-texel shadow shake every frame. The ground plane below
        // the camera is the correct anchor for a directional sun light.
        float cx = camPos.GetX();
        float cy = 0.0f;
        float cz = camPos.GetZ();
        float clipX = cx*lightVP_raw(0,0) + cy*lightVP_raw(1,0)
                    + cz*lightVP_raw(2,0) +    lightVP_raw(3,0);
        float clipY = cx*lightVP_raw(0,1) + cy*lightVP_raw(1,1)
                    + cz*lightVP_raw(2,1) +    lightVP_raw(3,1);
        float clipW = cx*lightVP_raw(0,3) + cy*lightVP_raw(1,3)
                    + cz*lightVP_raw(2,3) +    lightVP_raw(3,3);
        if (clipW != 0.0f) {
            float ndcX = clipX / clipW;
            float ndcY = clipY / clipW;
            float texX = ndcX * halfShadow;
            float texY = ndcY * halfShadow;
            float roundedX = std::round(texX);
            float roundedY = std::round(texY);
            float offsetNdcX = (roundedX - texX) / halfShadow;
            float offsetNdcY = (roundedY - texY) / halfShadow;
            lightProj(3, 0) += offsetNdcX;
            lightProj(3, 1) += offsetNdcY;
        }

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

    // Copy PREVIOUS frame's lightVP into the UBO. The shadow map currently
    // bound for sampling was rendered this frame using the renderer's
    // m_lightVP, which was set at the END of the previous UpdateAndBind call.
    // Sending the freshly-computed lightVP would mismatch the shadow texels
    // and produce a sub-texel shake on static geometry when the camera moves.
    if (m_hasPrevLightVP) {
        std::memcpy(ubo.LightVP, m_prevLightVP, sizeof(float) * 16);
    } else {
        std::memcpy(ubo.LightVP, &lightVP(0, 0), sizeof(float) * 16);
    }
    std::memcpy(m_prevLightVP, &lightVP(0, 0), sizeof(float) * 16);
    m_hasPrevLightVP = true;

    // NdcToShadow = InvViewProj * LightVP composed once on CPU so shadow
    // coords never round-trip through reconstructed world position (that
    // per-fragment path shimmers under camera rotation). Uses the same
    // LightVP the UBO carries (prev frame — matches the bound shadow map).
    {
        const Math::Matrix4& camV = Camera::GetMainViewMatrix();
        const Math::Matrix4& camP = Camera::GetMainProjectionMatrix();
        Math::Matrix4 camVP = camV * camP;
        float invVP[16];
        if (Invert4x4(&camVP(0, 0), invVP)) {
            Math::Matrix4 invVPm, lightVPm;
            std::memcpy(&invVPm(0, 0), invVP, sizeof(float) * 16);
            std::memcpy(&lightVPm(0, 0), ubo.LightVP, sizeof(float) * 16);
            Math::Matrix4 comp = invVPm * lightVPm;
            std::memcpy(ubo.NdcToShadow, &comp(0, 0), sizeof(float) * 16);
        } else {
            std::memcpy(ubo.NdcToShadow, ubo.LightVP, sizeof(float) * 16);
        }
    }

    ubo.ShadowBias = shadowLight ? shadowLight->GetShadowBias() : 0.0f;
    ubo.ShadowStrength = shadowLight ? shadowLight->GetShadowStrength() : 0.0f;
    ubo.ShadowTexelSize = 1.0f / 2048.0f;  // Match SHADOW_MAP_SIZE
    ubo.LightSize = shadowLight ? shadowLight->GetLightSize() : 0.0f;

    // Fog — distance gradient (horizon + zenith) and exponential height fog
    if (m_fogEnabled) {
        ubo.FogColor[0] = m_fogR;
        ubo.FogColor[1] = m_fogG;
        ubo.FogColor[2] = m_fogB;
        ubo.FogColor[3] = 1.0f;
        ubo.FogStart = m_fogStart;
        ubo.FogEnd = m_fogEnd;

        ubo.FogColorZenith[0] = m_fogZenithR;
        ubo.FogColorZenith[1] = m_fogZenithG;
        ubo.FogColorZenith[2] = m_fogZenithB;
        ubo.FogColorZenith[3] = 1.0f;

        ubo.HeightFogTop     = m_heightFogTop;
        ubo.HeightFogDensity = m_heightFogDensity;
        ubo.HeightFogFalloff = m_heightFogFalloff;
        ubo.HeightFogEnabled = m_heightFogEnabled ? 1.0f : 0.0f;
    } else {
        ubo.FogStart = 0.0f;
        ubo.FogEnd = 0.0f;
        ubo.HeightFogEnabled = 0.0f;
    }

    // Populate extra lights (fill, rim — non-shadow directional lights)
    ubo.NumExtraLights = 0;
    for (size_t i = 0; i < m_lights.GetSize() && ubo.NumExtraLights < 3; ++i) {
        Light* light = m_lights[i];
        if (!light || !light->IsEnabled()) continue;
        if (light == activeLight) continue; // already in primary slot
        auto* dlight = dynamic_cast<DirectionalLight*>(light);
        if (!dlight) continue;
        auto  eDir   = dlight->GetDirection();
        auto  eColor = dlight->GetColor();
        uint32_t idx = ubo.NumExtraLights;
        ubo.ExtraLightDir[idx][0] = eDir.GetX();
        ubo.ExtraLightDir[idx][1] = eDir.GetY();
        ubo.ExtraLightDir[idx][2] = eDir.GetZ();
        ubo.ExtraLightDir[idx][3] = 0.0f;
        ubo.ExtraLightColor[idx][0] = eColor.GetX();
        ubo.ExtraLightColor[idx][1] = eColor.GetY();
        ubo.ExtraLightColor[idx][2] = eColor.GetZ();
        ubo.ExtraLightColor[idx][3] = dlight->GetIntensity();
        ++ubo.NumExtraLights;
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
