#include <Camera/Camera.hpp>
#include <Core/Application.hpp>
#include <Core/GameBase.hpp>
#include <Core/SceneBase.hpp>
#include <Runtime/Skybox.hpp>
#include <algorithm>
#include <cstring>

#include "../../include/private/Graphics/OpenGL/OpenGLCubemapTexture.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLRenderer.hpp"
#include "Graphics/Common/ConstantBuffer.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

constexpr GLenum CUBE_FACE_COUNT = 6;
constexpr GLsizei STAND_IN_SIZE = 1;

}  // namespace

// ==================================================================
// Scene read for transparent formats (VertexLayoutDesc::transparentReadsScene)
// ==================================================================

/// Returns the active scene's skybox cubemap, or 0 when there is none.
GLuint OpenGLRenderer::FindSkyboxCubemap() const {
    auto* app = Application::GetInstance();
    auto* game = app ? app->GetGame() : nullptr;
    auto* scene = game ? game->GetActiveScene() : nullptr;
    auto* skybox = scene ? scene->GetSkybox() : nullptr;
    if (!skybox || !skybox->IsInitialized()) return 0;
    auto* cube =
        dynamic_cast<OpenGLCubemapTexture*>(skybox->GetCubemapTexture());
    return cube ? cube->GetGLTexture() : 0;
}

/// Snapshots the scene once per frame on the first scene-reading draw and
/// binds the snapshot, depth, sky, and SceneReadUBO for every such draw.
void OpenGLRenderer::PrepareSceneRead() {
    // Creation binds go to the units rebound below
    if (m_sceneFallbackCube == 0) {
        glGenTextures(1, &m_sceneFallbackCube);
        glActiveTexture(GL_TEXTURE0 + SCENE_SKY_UNIT);
        glBindTexture(GL_TEXTURE_CUBE_MAP, m_sceneFallbackCube);
        const uint32_t black = 0;
        for (GLenum face = 0; face < CUBE_FACE_COUNT; ++face)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA8,
                         STAND_IN_SIZE, STAND_IN_SIZE, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, &black);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);
    }
    if (m_sceneCopyTex == 0) {
        glGenTextures(1, &m_sceneCopyTex);
        glActiveTexture(GL_TEXTURE0 + SCENE_COLOR_UNIT);
        glBindTexture(GL_TEXTURE_2D, m_sceneCopyTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    if (m_sceneReadUBO == 0) {
        glGenBuffers(1, &m_sceneReadUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, m_sceneReadUBO);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(SceneReadUBO), nullptr,
                     GL_DYNAMIC_DRAW);
    }

    if (!m_sceneReadPrepared) {
        m_sceneReadPrepared = true;
        m_sceneSkyCube = FindSkyboxCubemap();
        const int w = m_gbufferWidth;
        const int h = m_gbufferHeight;
        const bool valid = m_gbufferCreated && IsDeferredEnabled() &&
                           m_gbufferDepth != 0 && w > 0 && h > 0;

        // Copy of the default framebuffer as drawn so far
        if (valid) {
            glActiveTexture(GL_TEXTURE0 + SCENE_COLOR_UNIT);
            glBindTexture(GL_TEXTURE_2D, m_sceneCopyTex);
            if (w != m_sceneCopyWidth || h != m_sceneCopyHeight) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, nullptr);
                m_sceneCopyWidth = w;
                m_sceneCopyHeight = h;
            }
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        }

        // Camera block
        const Math::Matrix4 viewProj =
            Camera::GetMainViewMatrix() * Camera::GetMainProjectionMatrix();
        SceneReadUBO ubo{};
        std::memcpy(ubo.ViewProj, &viewProj(0, 0), sizeof(ubo.ViewProj));
        std::memcpy(ubo.InvViewProj, m_cachedInvViewProj,
                    sizeof(ubo.InvViewProj));
        ubo.ScreenWidth = static_cast<float>(std::max(w, 1));
        ubo.ScreenHeight = static_cast<float>(std::max(h, 1));
        ubo.InvScreenWidth = 1.0f / ubo.ScreenWidth;
        ubo.InvScreenHeight = 1.0f / ubo.ScreenHeight;
        ubo.NearPlane = m_cachedNearPlane;
        ubo.FarPlane = m_cachedFarPlane;
        ubo.SceneValid = static_cast<float>(valid);
        ubo.SkyValid = static_cast<float>(m_sceneSkyCube != 0);
        glBindBuffer(GL_UNIFORM_BUFFER, m_sceneReadUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ubo), &ubo);
    }

    glBindBufferBase(GL_UNIFORM_BUFFER, SCENE_READ_UBO_BINDING, m_sceneReadUBO);
    glActiveTexture(GL_TEXTURE0 + SCENE_COLOR_UNIT);
    glBindTexture(GL_TEXTURE_2D, m_sceneCopyTex);
    glActiveTexture(GL_TEXTURE0 + SCENE_DEPTH_UNIT);
    glBindTexture(GL_TEXTURE_2D, m_gbufferDepth);
    glActiveTexture(GL_TEXTURE0 + SCENE_SKY_UNIT);
    glBindTexture(GL_TEXTURE_CUBE_MAP,
                  m_sceneSkyCube != 0 ? m_sceneSkyCube : m_sceneFallbackCube);
    glActiveTexture(GL_TEXTURE0);
}

/// Deletes the snapshot texture, stand-in cubemap, and UBO.
void OpenGLRenderer::CleanupSceneRead() {
    if (m_sceneCopyTex) glDeleteTextures(1, &m_sceneCopyTex);
    if (m_sceneFallbackCube) glDeleteTextures(1, &m_sceneFallbackCube);
    if (m_sceneReadUBO) glDeleteBuffers(1, &m_sceneReadUBO);
    m_sceneCopyTex = 0;
    m_sceneFallbackCube = 0;
    m_sceneReadUBO = 0;
    m_sceneCopyWidth = 0;
    m_sceneCopyHeight = 0;
}

}  // namespace RenderEngine
}  // namespace Sleak
