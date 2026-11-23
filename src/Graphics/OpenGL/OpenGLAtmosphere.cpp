#include "../../include/private/Graphics/OpenGL/OpenGLRenderer.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLShader.hpp"
#include "Core/Logger.hpp"

namespace Sleak {
namespace RenderEngine {

// Procedural atmosphere: sky probe and forward sky

namespace {

constexpr const char* FULLSCREEN_VERTEX =
    "assets/shaders/lighting_pass_gl.vert";
constexpr const char* PROBE_FRAGMENT =
    "assets/shaders/atmosphere_probe_gl.frag";
constexpr const char* FORWARD_SKY_FRAGMENT =
    "assets/shaders/atmosphere_sky_gl.frag";
constexpr const char* PROBE_FACE_UNIFORM = "uProbeFace";
constexpr GLsizei FULLSCREEN_VERTICES = 3;

}  // namespace

/// True while the deferred path lights the scene: its stem draws the sky.
bool OpenGLRenderer::AtmosphereSupported() const {
    return IsDeferredEnabled() && m_lightingShader && !m_atmosphereFailed;
}

/// Creates the probe cubemap, its framebuffer and its program on first use.
/// A failure is logged once and leaves the skybox drawing.
void OpenGLRenderer::EnsureSkyProbe() {
    if (m_skyProbeShader) return;
    const auto size = static_cast<GLsizei>(SKY_PROBE_SIZE);
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_skyProbeTex);
    glTextureStorage2D(m_skyProbeTex, static_cast<GLsizei>(SKY_PROBE_MIPS),
                       GL_RGBA16F, size, size);
    glTextureParameteri(m_skyProbeTex, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_skyProbeTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    for (GLenum axis :
         {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R})
        glTextureParameteri(m_skyProbeTex, axis, GL_CLAMP_TO_EDGE);
    glCreateFramebuffers(1, &m_skyProbeFBO);
    glNamedFramebufferTextureLayer(m_skyProbeFBO, GL_COLOR_ATTACHMENT0,
                                   m_skyProbeTex, 0, 0);

    m_skyProbeShader = new OpenGLShader();
    if (glCheckNamedFramebufferStatus(m_skyProbeFBO, GL_FRAMEBUFFER) ==
            GL_FRAMEBUFFER_COMPLETE &&
        m_skyProbeShader->compile(FULLSCREEN_VERTEX, PROBE_FRAGMENT))
        return;
    SLEAK_ERROR("Atmosphere: OpenGL sky probe failed, the skybox stays");
    CleanupSkyProbe();
    m_atmosphereFailed = true;
}

/// Deletes the probe, the forward sky and the cloud noise and forgets a
/// failed probe.
void OpenGLRenderer::CleanupSkyProbe() {
    if (m_skyProbeTex) glDeleteTextures(1, &m_skyProbeTex);
    if (m_skyProbeFBO) glDeleteFramebuffers(1, &m_skyProbeFBO);
    delete m_skyProbeShader;
    m_skyProbeTex = 0;
    m_skyProbeFBO = 0;
    m_skyProbeShader = nullptr;
    m_atmosphereFailed = false;
    if (m_forwardSkyVAO) glDeleteVertexArrays(1, &m_forwardSkyVAO);
    delete m_forwardSkyShader;
    m_forwardSkyVAO = 0;
    m_forwardSkyShader = nullptr;
    ReleaseCloudNoise();
}

/// Binds the cloud noise, or nothing, at its unit.
void OpenGLRenderer::BindCloudNoise() const {
    if (m_cloudNoise)
        m_cloudNoise->Bind(CLOUD_NOISE_UNIT);
    else
        glBindTextureUnit(CLOUD_NOISE_UNIT, 0);
}

/// Draws the sky gradient and clouds into the probe faces and rebuilds their
/// mips, so this frame's reflections see this frame's sky. The lighting pass
/// that follows sets its own target, depth, blend and vertex array state.
void OpenGLRenderer::RenderSkyProbe() {
    if (!DrawsAtmosphere() || !m_skyProbeShader) return;
    // shortcut: redrawn every frame, add a change test if a frame profile
    // shows the probe
    const auto size = static_cast<GLsizei>(SKY_PROBE_SIZE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_skyProbeFBO);
    glViewport(0, 0, size, size);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    m_skyProbeShader->bind();
    const GLint faceUniform = glGetUniformLocation(
        m_skyProbeShader->GetProgram(), PROBE_FACE_UNIFORM);
    glBindVertexArray(m_lightingVAO);
    BindCloudNoise();
    for (GLuint face = 0; face < CUBE_FACES; ++face) {
        glNamedFramebufferTextureLayer(m_skyProbeFBO, GL_COLOR_ATTACHMENT0,
                                       m_skyProbeTex, 0,
                                       static_cast<GLint>(face));
        glUniform1ui(faceUniform, face);
        glDrawArrays(GL_TRIANGLES, 0, FULLSCREEN_VERTICES);
    }
    glGenerateTextureMipmap(m_skyProbeTex);
    glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
}

/// True on the forward path until the forward sky fails to build.
bool OpenGLRenderer::ForwardSkySupported() const {
    return !IsDeferredEnabled() && !m_forwardSkyFailed;
}

/// Compiles the forward sky program and its empty vertex array on first use.
bool OpenGLRenderer::EnsureForwardSky() {
    if (m_forwardSkyShader) return true;
    auto* shader = new OpenGLShader();
    if (!shader->compile(FULLSCREEN_VERTEX, FORWARD_SKY_FRAGMENT)) {
        delete shader;
        SLEAK_ERROR("Atmosphere: OpenGL forward sky failed, the skybox stays");
        m_forwardSkyFailed = true;
        return false;
    }
    m_forwardSkyShader = shader;
    glCreateVertexArrays(1, &m_forwardSkyVAO);
    return true;
}

/// Draws the atmosphere's sky as the forward path's first draw: the scene
/// covers it, so it needs no depth test.
void OpenGLRenderer::RenderForwardSky() {
    if (!DrawsForwardSky() || !EnsureForwardSky()) return;
    m_forwardSkyShader->bind();
    BindCloudNoise();
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(m_forwardSkyVAO);
    glDrawArrays(GL_TRIANGLES, 0, FULLSCREEN_VERTICES);
    glBindVertexArray(m_VAO);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace RenderEngine
}  // namespace Sleak
