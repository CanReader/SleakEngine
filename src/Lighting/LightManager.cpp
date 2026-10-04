#include <Lighting/LightManager.hpp>
#include <Lighting/Light.hpp>
#include <Lighting/DirectionalLight.hpp>
#include <Graphics/Common/ConstantBuffer.hpp>
#include <Graphics/Common/ResourceManager.hpp>
#include <Graphics/Common/BufferBase.hpp>
#include <Graphics/Common/Renderer.hpp>
#include <Graphics/Common/RenderContext.hpp>
#include <Camera/Camera.hpp>
#include <Core/Window.hpp>
#include <Core/Application.hpp>
#include <Core/CommandLine.hpp>
#include <Math/Matrix.hpp>
#include <Core/Timer.hpp>
#include <Core/Logger.hpp>
#include <cstring>
#include <cmath>

namespace {
/// 4x4 row-major matrix inverse via cofactors (Cramer's rule).
/// Returns false if matrix is singular.
static bool Invert4x4(const float m[16], float inv[16]) {
    float inv0  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    float inv4  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    float inv8  =  m[4]*m[9] *m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    float inv12 = -m[4]*m[9] *m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

    float det = m[0]*inv0 + m[1]*inv4 + m[2]*inv8 + m[3]*inv12;
    if (std::fabs(det) < 1e-8f) return false;
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

constexpr float kCascadeBlend = 0.1f;
constexpr float kCascadePadTexels = 8.0f;

/// Near plane of the main camera, read back from its projection matrix.
float MainCameraNear() {
    const auto& P = Sleak::Camera::GetMainProjectionMatrix();
    if (P(2, 3) == 0.0f || P(2, 2) == 0.0f) return 0.1f;
    float n = -P(3, 2) / P(2, 2);
    return n > 0.0f ? n : 0.1f;
}

/// Fits one sphere per cascade around the camera (practical split radii) and
/// builds a texel-snapped orthographic light view-projection for each.
void BuildShadowCascades(const Sleak::DirectionalLight& light,
                         const Sleak::Math::Vector3D& camPos, uint32_t count,
                         uint32_t resolution, float outVP[][16],
                         float outSplits[]) {
    using namespace Sleak;
    float maxDist = light.GetShadowMaxDistance();
    if (maxDist <= 0.0f) maxDist = light.GetShadowFrustumSize();
    const float nearDist = std::clamp(MainCameraNear(), 0.01f, maxDist * 0.5f);
    const float lambda = std::clamp(light.GetShadowSplitLambda(), 0.0f, 1.0f);

    // Fixed light orientation with no translation: the cascades only move
    // in light space, where they are snapped to whole texels.
    const auto dir = light.GetDirection();
    Math::Vector<float, 3> ld({dir.GetX(), dir.GetY(), dir.GetZ()});
    Math::Vector<float, 3> up =
        (std::fabs(dir.GetY()) > 0.999f)
            ? Math::Vector<float, 3>({0.0f, 0.0f, 1.0f})
            : Math::Vector<float, 3>({0.0f, 1.0f, 0.0f});
    Math::Matrix4 lightView = Math::Matrix4::LookTo(
        Math::Vector<float, 3>({0.0f, 0.0f, 0.0f}), ld, up);

    auto toLight = [&](int axis) {
        return camPos.GetX() * lightView(0, axis) +
               camPos.GetY() * lightView(1, axis) +
               camPos.GetZ() * lightView(2, axis);
    };
    const float cx = toLight(0), cy = toLight(1), cz = toLight(2);

    const float res = static_cast<float>(resolution);
    const float pad = res / (res - 2.0f * kCascadePadTexels);

    for (uint32_t i = 0; i < count; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(count);
        const float logSplit = nearDist * std::pow(maxDist / nearDist, p);
        const float uniSplit = nearDist + (maxDist - nearDist) * p;
        const float split =
            (i + 1 == count) ? maxDist
                             : lambda * logSplit + (1.0f - lambda) * uniSplit;
        outSplits[i] = split;

        const float halfExtent = split * pad;
        const float texel = 2.0f * halfExtent / res;
        const float x = std::round(cx / texel) * texel;
        const float y = std::round(cy / texel) * texel;

        const float pull = std::max(light.GetShadowDistance(), halfExtent);
        const float zNear = cz - pull + light.GetShadowNearPlane();
        const float zFar =
            cz - pull + std::max(light.GetShadowFarPlane(), pull + halfExtent);

        // Vulkan-style [0,1] depth; translations live in row 3 because the
        // engine is row-major and GLSL reads the matrix transposed.
        Math::Matrix4 proj = Math::Matrix4::Identity();
        proj(0, 0) = 1.0f / halfExtent;
        proj(1, 1) = 1.0f / halfExtent;
        proj(2, 2) = 1.0f / (zFar - zNear);
        proj(3, 0) = -x / halfExtent;
        proj(3, 1) = -y / halfExtent;
        proj(3, 2) = -zNear / (zFar - zNear);

        Math::Matrix4 vp = lightView * proj;
        std::memcpy(outVP[i], &vp(0, 0), sizeof(float) * 16);
    }
}

/// Projection near and far planes of the main camera.
void MainCameraNearFar(float& nearP, float& farP) {
    const auto& P = Sleak::Camera::GetMainProjectionMatrix();
    nearP = MainCameraNear();
    farP = (P(2, 2) > 1.0f) ? P(2, 2) * nearP / (P(2, 2) - 1.0f) : 1000.0f;
}

/// Inclusive [lo, hi] tile range covered by an NDC interval.
void NdcToTiles(float ndcMin, float ndcMax, uint32_t tiles, uint32_t& lo,
                uint32_t& hi) {
    auto toTile = [tiles](float ndc) {
        float t = (ndc * 0.5f + 0.5f) * static_cast<float>(tiles);
        return static_cast<uint32_t>(
            std::clamp(t, 0.0f, static_cast<float>(tiles - 1)));
    };
    lo = toTile(ndcMin);
    hi = toTile(ndcMax);
}
} // namespace

namespace Sleak {

/// Per-frame buffers reused by the light sort and the cluster build.
struct LightManager::ClusterScratch {
    std::vector<std::pair<float, Light*>> sortedLocal;
    std::vector<RenderEngine::LightGPUEntry> gpuLights;
    std::vector<uint32_t> pairs;  // cluster << 10 | light
    std::vector<uint32_t> cells;  // (offset, count) per cluster
    std::vector<uint32_t> indices;
    bool warnedOverflow = false;
};

LightManager::LightManager() : m_clusters(new ClusterScratch()) {}

LightManager::~LightManager() { delete m_clusters; }

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

    m_lights.add(light);
    if (auto* dir = dynamic_cast<DirectionalLight*>(light))
        m_directionalLights.add(dir);
    else
        m_localLights.add(light);
}

void LightManager::UnregisterLight(Light* light) {
    if (!light) return;
    int index = m_lights.indexOf(light);
    if (index == -1) return;
    m_lights.erase(index);

    for (size_t i = 0; i < m_directionalLights.GetSize(); ++i) {
        if (m_directionalLights[i] == light) {
            m_directionalLights.erase(static_cast<int>(i));
            return;
        }
    }
    int local = m_localLights.indexOf(light);
    if (local != -1) m_localLights.erase(local);
}

DirectionalLight* LightManager::FindMainDirectionalLight(
    bool& castsShadow) const {
    DirectionalLight* firstEnabled = nullptr;
    for (size_t i = 0; i < m_directionalLights.GetSize(); ++i) {
        DirectionalLight* light = m_directionalLights[i];
        if (!light->IsEnabled()) continue;
        if (light->GetCastShadows()) {
            castsShadow = true;
            return light;
        }
        if (!firstEnabled) firstEnabled = light;
    }
    castsShadow = false;
    return firstEnabled;
}

void LightManager::SortLocalLights(const Math::Vector3D& camPos) {
    auto& sorted = m_clusters->sortedLocal;
    sorted.clear();
    for (size_t i = 0; i < m_localLights.GetSize(); ++i) {
        Light* light = m_localLights[i];
        if (!light->IsEnabled()) continue;
        const auto gpu = light->BuildGPUData();
        const float dx = gpu.PositionX - camPos.GetX();
        const float dy = gpu.PositionY - camPos.GetY();
        const float dz = gpu.PositionZ - camPos.GetZ();
        sorted.emplace_back(dx * dx + dy * dy + dz * dz, light);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
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

    // Forward list: the main directional light first (shaders shadow slot
    // 0), the other directional lights, then the nearest local lights.
    bool mainCastsShadow = false;
    DirectionalLight* mainLight = FindMainDirectionalLight(mainCastsShadow);
    SortLocalLights(camPos);

    uint32_t count = 0;
    auto pushForward = [&](const Light* light) {
        if (count < RenderEngine::MAX_LIGHTS)
            cbData.Lights[count++] = light->BuildGPUData();
    };
    if (mainLight) pushForward(mainLight);
    for (size_t i = 0; i < m_directionalLights.GetSize(); ++i) {
        DirectionalLight* light = m_directionalLights[i];
        if (light != mainLight && light->IsEnabled()) pushForward(light);
    }
    for (const auto& entry : m_clusters->sortedLocal) pushForward(entry.second);
    cbData.NumActiveLights = count;

    // Update and bind at slot 2
    m_lightBuffer->Update(&cbData, sizeof(cbData));
    m_lightBuffer->Update();

    // Update shadow data for Vulkan renderer
    UpdateShadowData();

    // Update deferred CB (InvViewProj + screen size) for the lighting pass
    UpdateDeferredCB();

    UpdateClusters(mainLight);
}

void LightManager::UpdateShadowData() {
    auto* app = Application::GetInstance();
    if (!app) return;
    auto* renderer = app->GetRenderer();
    if (!renderer) return;

    bool castsShadow = false;
    DirectionalLight* activeLight = FindMainDirectionalLight(castsShadow);
    DirectionalLight* shadowLight = castsShadow ? activeLight : nullptr;

    // Tell the renderer whether the shadow pass should run
    renderer->SetShadowPassEnabled(shadowLight != nullptr);

    if (!activeLight) {
        static bool warned = false;
        if (!warned) { SLEAK_WARN("UpdateShadowData: No directional light found!"); warned = true; }
        return;
    }

    auto dir = activeLight->GetDirection();
    auto color = activeLight->GetColor();
    float intensity = activeLight->GetIntensity();

    Math::Matrix4 lightVP = Math::Matrix4::Identity();
    float cascadeVP[RenderEngine::MAX_SHADOW_CASCADES][16] = {};
    float cascadeSplits[RenderEngine::MAX_SHADOW_CASCADES] = {};
    uint32_t cascadeCount = 0;

    if (shadowLight) {
        cascadeCount = std::clamp(shadowLight->GetShadowCascadeCount(), 1u,
                                  RenderEngine::MAX_SHADOW_CASCADES);
        renderer->SetShadowCascadeCount(cascadeCount);
        BuildShadowCascades(*shadowLight, Camera::GetMainCameraPosition(),
                            cascadeCount, renderer->GetShadowMapResolution(),
                            cascadeVP, cascadeSplits);
    }

    // DIAG --shadowfreeze: latch the first cascades forever. If shadows still
    // shimmer with a frozen frustum, the cause is screen-space, not the
    // frustum-follow chain.
    {
        static const bool s_freeze = CommandLine::HasFlag("--shadowfreeze");
        static bool s_latched = false;
        static float s_frozenVP[RenderEngine::MAX_SHADOW_CASCADES][16];
        if (s_freeze && shadowLight) {
            if (!s_latched) {
                std::memcpy(s_frozenVP, cascadeVP, sizeof(s_frozenVP));
                s_latched = true;
                SLEAK_WARN("shadowfreeze: light frustum latched");
            } else {
                std::memcpy(cascadeVP, s_frozenVP, sizeof(s_frozenVP));
            }
        }
    }

    // The last cascade doubles as the single map for older shaders.
    if (cascadeCount > 0) {
        std::memcpy(&lightVP(0, 0), cascadeVP[cascadeCount - 1],
                    sizeof(float) * 16);
    }
    renderer->SetLightVP(&lightVP(0, 0));
    renderer->SetShadowCascades(&cascadeVP[0][0], cascadeCount);

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

    // CURRENT lightVP — renderers stage SetLightVP and commit at BeginRender,
    // so this frame's shadow map IS rendered with this matrix. The old
    // prev-frame copy lagged sampling one frame behind the map (shadow shake
    // while the camera moved).
    std::memcpy(ubo.LightVP, &lightVP(0, 0), sizeof(float) * 16);

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
    ubo.ShadowTexelSize =
        1.0f / static_cast<float>(renderer->GetShadowMapResolution());
    ubo.LightSize = shadowLight ? shadowLight->GetLightSize() : 0.0f;

    std::memcpy(ubo.CascadeVP, cascadeVP, sizeof(ubo.CascadeVP));
    std::memcpy(ubo.CascadeSplits, cascadeSplits, sizeof(ubo.CascadeSplits));
    ubo.CascadeCount = cascadeCount;
    ubo.CascadeBlend = kCascadeBlend;

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
    for (size_t i = 0;
         i < m_directionalLights.GetSize() && ubo.NumExtraLights < 3; ++i) {
        DirectionalLight* dlight = m_directionalLights[i];
        if (!dlight->IsEnabled() || dlight == activeLight) continue;
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

void LightManager::UpdateClusters(const DirectionalLight* mainLight) {
    using namespace RenderEngine;
    auto* app = Application::GetInstance();
    auto* renderer = app ? app->GetRenderer() : nullptr;
    if (!renderer || !renderer->SupportsClusteredLights()) return;

    auto& sc = *m_clusters;
    sc.gpuLights.clear();
    for (size_t i = 0; i < m_directionalLights.GetSize(); ++i) {
        DirectionalLight* light = m_directionalLights[i];
        if (light != mainLight && light->IsEnabled() &&
            sc.gpuLights.size() < MAX_CLUSTERED_LIGHTS)
            sc.gpuLights.push_back(light->BuildGPUData());
    }
    const uint32_t globalCount = static_cast<uint32_t>(sc.gpuLights.size());
    for (const auto& entry : sc.sortedLocal) {
        if (sc.gpuLights.size() >= MAX_CLUSTERED_LIGHTS) break;
        sc.gpuLights.push_back(entry.second->BuildGPUData());
    }
    const uint32_t lightCount = static_cast<uint32_t>(sc.gpuLights.size());

    // Exponential depth slices between the camera near plane (at least 1m,
    // anything closer lands in slice 0) and the far plane.
    float nearP, farP;
    MainCameraNearFar(nearP, farP);
    const float zNear = std::max(nearP, 1.0f);
    const float zFar = std::max(farP, zNear * 2.0f);
    const float logRange = std::log(zFar / zNear);
    const float zScale = static_cast<float>(CLUSTER_GRID_Z) / logRange;
    const float zBias = -zScale * std::log(zNear);
    auto sliceOf = [&](float z) {
        float k = std::log(std::max(z, 1e-3f)) * zScale + zBias;
        return static_cast<uint32_t>(
            std::clamp(k, 0.0f, static_cast<float>(CLUSTER_GRID_Z - 1)));
    };
    auto sliceNear = [&](uint32_t k) {
        return k == 0 ? 0.0f : zNear * std::exp(static_cast<float>(k) / zScale);
    };

    const Math::Matrix4& V = Camera::GetMainViewMatrix();
    const Math::Matrix4& P = Camera::GetMainProjectionMatrix();
    const float p00 = P(0, 0), p11 = P(1, 1);

    ClusterParamsGPU params{};
    params.ViewZ[0] = V(0, 2);
    params.ViewZ[1] = V(1, 2);
    params.ViewZ[2] = V(2, 2);
    params.ViewZ[3] = V(3, 2);
    params.GridX = CLUSTER_GRID_X;
    params.GridY = CLUSTER_GRID_Y;
    params.GridZ = CLUSTER_GRID_Z;
    params.GlobalLightCount = globalCount;
    params.ZScale = zScale;
    params.ZBias = zBias;
    params.LocalLightCount = lightCount - globalCount;

    // Pass 1: (cluster, light) pairs from each light's sphere against the
    // view-space box of every cluster its screen bounds and depth touch.
    sc.pairs.clear();
    for (uint32_t li = globalCount; li < lightCount; ++li) {
        const LightGPUEntry& l = sc.gpuLights[li];
        const float r = l.Range;
        if (r <= 0.0f) continue;
        const float px = l.PositionX, py = l.PositionY, pz = l.PositionZ;
        const float vx = px * V(0, 0) + py * V(1, 0) + pz * V(2, 0) + V(3, 0);
        const float vy = px * V(0, 1) + py * V(1, 1) + pz * V(2, 1) + V(3, 1);
        const float vz = px * V(0, 2) + py * V(1, 2) + pz * V(2, 2) + V(3, 2);
        if (vz + r <= nearP) continue;

        const float z0 = std::max(vz - r, nearP);
        const float z1 = vz + r;
        float ndc[4] = {1.0f, -1.0f, 1.0f, -1.0f};  // xmin xmax ymin ymax
        for (float z : {z0, z1}) {
            for (float x : {vx - r, vx + r}) {
                ndc[0] = std::min(ndc[0], p00 * x / z);
                ndc[1] = std::max(ndc[1], p00 * x / z);
            }
            for (float y : {vy - r, vy + r}) {
                ndc[2] = std::min(ndc[2], p11 * y / z);
                ndc[3] = std::max(ndc[3], p11 * y / z);
            }
        }
        if (ndc[1] < -1.0f || ndc[0] > 1.0f || ndc[3] < -1.0f || ndc[2] > 1.0f)
            continue;

        uint32_t tx0, tx1, ty0, ty1;
        NdcToTiles(ndc[0], ndc[1], CLUSTER_GRID_X, tx0, tx1);
        NdcToTiles(ndc[2], ndc[3], CLUSTER_GRID_Y, ty0, ty1);
        const uint32_t k0 = sliceOf(z0), k1 = sliceOf(z1);

        for (uint32_t k = k0; k <= k1; ++k) {
            const float cz0 = sliceNear(k);
            const float cz1 = (k + 1 == CLUSTER_GRID_Z) ? std::max(z1, cz0)
                                                        : sliceNear(k + 1);
            const float dz = vz < cz0 ? cz0 - vz : (vz > cz1 ? vz - cz1 : 0.0f);
            for (uint32_t ty = ty0; ty <= ty1; ++ty) {
                const float ny0 = -1.0f + 2.0f * ty / CLUSTER_GRID_Y;
                const float ny1 = -1.0f + 2.0f * (ty + 1) / CLUSTER_GRID_Y;
                const float y0 = std::min(ny0 * cz0, ny0 * cz1) / p11;
                const float y1 = std::max(ny1 * cz0, ny1 * cz1) / p11;
                const float dy = vy < y0 ? y0 - vy : (vy > y1 ? vy - y1 : 0.0f);
                for (uint32_t tx = tx0; tx <= tx1; ++tx) {
                    const float nx0 = -1.0f + 2.0f * tx / CLUSTER_GRID_X;
                    const float nx1 = -1.0f + 2.0f * (tx + 1) / CLUSTER_GRID_X;
                    const float x0 = std::min(nx0 * cz0, nx0 * cz1) / p00;
                    const float x1 = std::max(nx1 * cz0, nx1 * cz1) / p00;
                    const float dx =
                        vx < x0 ? x0 - vx : (vx > x1 ? vx - x1 : 0.0f);
                    if (dx * dx + dy * dy + dz * dz > r * r) continue;
                    const uint32_t cell =
                        (k * CLUSTER_GRID_Y + ty) * CLUSTER_GRID_X + tx;
                    sc.pairs.push_back(cell << 10 | li);
                }
            }
        }
    }

    // Pass 2: counting sort the pairs into per-cluster runs.
    sc.cells.assign(CLUSTER_COUNT * 2, 0);
    size_t total = sc.pairs.size();
    if (total > MAX_CLUSTER_LIGHT_INDICES) {
        if (!sc.warnedOverflow) {
            SLEAK_WARN(
                "Clustered lights: {} light references exceed the {} "
                "index budget, dropping the rest",
                total, MAX_CLUSTER_LIGHT_INDICES);
            sc.warnedOverflow = true;
        }
        total = MAX_CLUSTER_LIGHT_INDICES;
    }
    for (size_t i = 0; i < total; ++i) ++sc.cells[(sc.pairs[i] >> 10) * 2 + 1];
    uint32_t offset = 0;
    for (uint32_t c = 0; c < CLUSTER_COUNT; ++c) {
        sc.cells[c * 2] = offset;
        offset += sc.cells[c * 2 + 1];
        sc.cells[c * 2 + 1] = 0;
    }
    sc.indices.resize(total);
    for (size_t i = 0; i < total; ++i) {
        const uint32_t cell = sc.pairs[i] >> 10;
        uint32_t& n = sc.cells[cell * 2 + 1];
        sc.indices[sc.cells[cell * 2] + n++] = sc.pairs[i] & 1023u;
    }

    renderer->UpdateClusteredLights(params, sc.gpuLights.data(),
                                    sc.cells.data(), sc.indices.data(),
                                    static_cast<uint32_t>(total));
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
