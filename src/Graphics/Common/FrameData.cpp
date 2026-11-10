#include <Camera/Camera.hpp>
#include <Core/Application.hpp>
#include <Core/SceneBase.hpp>
#include <algorithm>
#include <cstring>

#include "../../include/private/Graphics/Common/FrameDataBuilder.hpp"
#include "../../include/private/Graphics/Common/MatrixMath.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

// Planes used until a scene camera exists
constexpr float FALLBACK_NEAR_PLANE = 0.1f;
constexpr float FALLBACK_FAR_PLANE = 2000.0f;
constexpr int AXES = 3;
constexpr int W_COMPONENT = 3;
constexpr int VEC4_SIZE = 4;
constexpr int MATRIX_DIM = 4;
constexpr int MATRIX_SIZE = MATRIX_DIM * MATRIX_DIM;
// Row vectors: the translation is the last row
constexpr int TRANSLATION_ROW = (MATRIX_DIM - 1) * MATRIX_DIM;
// Jitter holds xy for this frame, then xy for the previous one
constexpr int JITTER_AXES = 2;
constexpr int PREVIOUS_JITTER = 2;

/// Row-major translation matrix for row vectors.
void TranslationMatrix(const double offset[AXES], double out[MATRIX_SIZE]) {
    for (int i = 0; i < MATRIX_SIZE; ++i)
        out[i] = (i / MATRIX_DIM == i % MATRIX_DIM) ? 1.0 : 0.0;
    for (int axis = 0; axis < AXES; ++axis)
        out[TRANSLATION_ROW + axis] = offset[axis];
}

/// Widens a row-major float matrix.
void ToDouble(const float in[MATRIX_SIZE], double out[MATRIX_SIZE]) {
    for (int i = 0; i < MATRIX_SIZE; ++i) out[i] = in[i];
}

/// Narrows a row-major double matrix.
void ToFloat(const double in[MATRIX_SIZE], float out[MATRIX_SIZE]) {
    for (int i = 0; i < MATRIX_SIZE; ++i) out[i] = static_cast<float>(in[i]);
}

}  // namespace

void GetMainCameraPlanes(float& nearPlane, float& farPlane) {
    nearPlane = FALLBACK_NEAR_PLANE;
    farPlane = FALLBACK_FAR_PLANE;
    Application* app = Application::GetInstance();
    GameBase* game = app ? app->GetGame() : nullptr;
    SceneBase* scene = game ? game->GetActiveScene() : nullptr;
    Camera* camera = scene ? scene->GetActiveCamera() : nullptr;
    if (!camera) return;
    nearPlane = camera->GetNearPlane();
    farPlane = camera->GetFarPlane();
}

void FrameDataBuilder::SetLight(const ShadowLightUBO& light) {
    for (int axis = 0; axis < AXES; ++axis) {
        m_lightDir[axis] = -light.LightDir[axis];
        m_lightColor[axis] = light.LightColor[axis];
    }
    m_lightDir[W_COMPONENT] = light.LightColor[W_COMPONENT];
    m_lightColor[W_COMPONENT] = light.ShadowStrength;
    std::copy_n(light.Ambient, VEC4_SIZE, m_ambient);
}

bool FrameDataBuilder::SetUserData(const void* data, uint32_t size) {
    if (size > m_userData.size() || (size > 0 && !data)) return false;
    m_userData.fill(0);
    if (size > 0) std::memcpy(m_userData.data(), data, size);
    return true;
}

void FrameDataBuilder::Build(const FrameTargetInfo& target, FrameData& out) {
    out = {};
    out.Version = FRAME_DATA_VERSION;
    if (const Application* app = Application::GetInstance()) {
        out.FrameIndex = static_cast<uint32_t>(app->GetFrameIndex());
        out.Time = app->GetEngineTime();
        out.DeltaTime = app->GetDeltaTime();
    }

    const float width = static_cast<float>(std::max(target.width, 1u));
    const float height = static_cast<float>(std::max(target.height, 1u));
    out.Screen[0] = width;
    out.Screen[1] = height;
    out.Screen[2] = 1.0f / width;
    out.Screen[3] = 1.0f / height;

    for (int axis = 0; axis < JITTER_AXES; ++axis) {
        out.Jitter[axis] = target.jitter[axis];
        out.Jitter[PREVIOUS_JITTER + axis] =
            m_hasPrevious ? m_prevJitter[axis] : target.jitter[axis];
    }

    float nearPlane = FALLBACK_NEAR_PLANE;
    float farPlane = FALLBACK_FAR_PLANE;
    GetMainCameraPlanes(nearPlane, farPlane);
    const Math::Vector3D& camera = Camera::GetMainCameraPosition();
    const double cameraPos[AXES] = {camera.GetX(), camera.GetY(),
                                    camera.GetZ()};
    const double* prevCamera = m_hasPrevious ? m_prevCameraPos : cameraPos;
    for (int axis = 0; axis < AXES; ++axis) {
        out.CameraPos[axis] = static_cast<float>(cameraPos[axis]);
        out.PrevCameraPos[axis] = static_cast<float>(prevCamera[axis]);
    }
    out.CameraPos[W_COMPONENT] = nearPlane;
    out.PrevCameraPos[W_COMPONENT] = farPlane;

    const Math::Matrix4& view = Camera::GetMainViewMatrix();
    const Math::Matrix4& proj = Camera::GetMainProjectionMatrix();
    std::memcpy(out.View, &view(0, 0), sizeof(out.View));
    std::memcpy(out.Proj, &proj(0, 0), sizeof(out.Proj));

    double viewD[MATRIX_SIZE], projD[MATRIX_SIZE], viewProj[MATRIX_SIZE];
    ToDouble(out.View, viewD);
    ToDouble(out.Proj, projD);
    MatMul4(viewD, projD, viewProj);
    ToFloat(viewProj, out.ViewProj);

    double invViewProj[MATRIX_SIZE], shift[MATRIX_SIZE], rel[MATRIX_SIZE];
    const double toCamera[AXES] = {-cameraPos[0], -cameraPos[1], -cameraPos[2]};
    if (InvertMat4(viewProj, invViewProj)) {
        TranslationMatrix(toCamera, shift);
        MatMul4(invViewProj, shift, rel);
        ToFloat(rel, out.InvViewProjRel);
    }
    TranslationMatrix(cameraPos, shift);
    MatMul4(shift, m_hasPrevious ? m_prevViewProj : viewProj, rel);
    ToFloat(rel, out.PrevViewProjRel);

    std::copy_n(m_lightDir, VEC4_SIZE, out.LightDir);
    std::copy_n(m_lightColor, VEC4_SIZE, out.LightColor);
    std::copy_n(m_ambient, VEC4_SIZE, out.Ambient);
    out.Flags[0] = target.featureFlags;
#ifndef NDEBUG
    out.Flags[0] |= FrameFlagGuard;
#endif
    out.Flags[1] = target.cascadeCount;
    out.Flags[FRAME_FLAGS_TONEMAP_INDEX] = target.output.op;
    out.Output[0] = target.output.exposure;
    out.Output[1] = target.output.gamma;
    out.Output[2] = target.output.saturation;
    out.Output[3] = target.output.dither;
    out.OutputCurve[0] = target.output.curve;
    std::copy_n(target.output.white, VEC4_SIZE, out.WhitePath);
    std::copy_n(target.output.whiteInverse, VEC4_SIZE, out.WhitePathInverse);

    std::copy_n(viewProj, MATRIX_SIZE, m_prevViewProj);
    std::copy_n(cameraPos, AXES, m_prevCameraPos);
    std::copy_n(target.jitter, JITTER_AXES, m_prevJitter);
    m_hasPrevious = true;
}

}  // namespace RenderEngine
}  // namespace Sleak
