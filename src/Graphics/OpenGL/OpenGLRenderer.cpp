#include "../../include/private/Graphics/OpenGL/OpenGLRenderer.hpp"

#include <SDL3/SDL.h>

#include <Camera/Camera.hpp>
#include <Core/Application.hpp>
#include <Core/GameBase.hpp>
#include <Core/SceneBase.hpp>
#include <Runtime/Material.hpp>
#include <Runtime/MeshData.hpp>
#include <Runtime/Skybox.hpp>
#include <Runtime/VertexLayout.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "../../include/private/Graphics/OpenGL/OpenGLBuffer.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLCubemapTexture.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLIBL.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLShader.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLTexture.hpp"
#include "Graphics/Common/ConstantBuffer.hpp"
#include "Graphics/Common/FrameCapture.hpp"
#include "Graphics/Common/RenderCommandQueue.hpp"
#include "Graphics/Common/ResourceManager.hpp"
#include "Graphics/Common/SSAOKernel.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

constexpr GLuint VERTEX_BUFFER_BINDING = 0;
constexpr uint32_t DEFAULT_VERTEX_LAYOUT = 0;
constexpr uint32_t ATTRIB_MASK_BITS = 32;

// Default Vertex attribute locations
constexpr GLuint ATTRIB_POSITION = 0;
constexpr GLuint ATTRIB_NORMAL = 1;
constexpr GLuint ATTRIB_TANGENT = 2;
constexpr GLuint ATTRIB_COLOR = 3;
constexpr GLuint ATTRIB_UV = 4;
constexpr GLuint ATTRIB_BONE_IDS = 5;
constexpr GLuint ATTRIB_BONE_WEIGHTS = 6;

// Transform CB layout read by the shadow pass: [WVP][World]
constexpr int MAT4_DIM = 4;
constexpr size_t MAT4_FLOATS = MAT4_DIM * MAT4_DIM;
constexpr size_t MAT4_BYTES = MAT4_FLOATS * sizeof(float);
constexpr size_t TRANSFORM_CB_BYTES = 2 * MAT4_BYTES;

// Pixel pack alignment: tight rows for the capture, GL's default afterwards
constexpr GLint CAPTURE_PACK_ALIGNMENT = 1;
constexpr GLint DEFAULT_PACK_ALIGNMENT = 4;

// RT0 albedo + AO: UNORM by default, sRGB with SetGBufferAlbedoSRGB
constexpr GLenum GBUFFER_ALBEDO_FORMAT = GL_RGBA8;
constexpr GLenum GBUFFER_ALBEDO_SRGB_FORMAT = GL_SRGB8_ALPHA8;
// GBuffer color targets; RT3 holds motion, game channel and shading model
constexpr GLsizei GBUFFER_COLOR_COUNT = 4;
constexpr GLuint GBUFFER_MOTION_MODEL_INDEX = 3;
constexpr GLenum GBUFFER_DRAW_BUFFERS[GBUFFER_COLOR_COUNT] = {
    GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
    GL_COLOR_ATTACHMENT3};
// Lighting pass unit of RT3 (sleak/lighting_inputs.glsl SLEAK_MOTION_MODEL_GL)
constexpr GLenum GBUFFER_MOTION_MODEL_TEXTURE_UNIT = GL_TEXTURE12;
// Unit alpha-tested shadow programs read the caster diffuse from
constexpr GLuint SHADOW_MATERIAL_UNIT = 0;

// Shader files; custom-format stems resolve to <stem>_gl.vert/.frag
constexpr const char* SHADER_DIR = "assets/shaders/";
constexpr const char* GL_VERTEX_SUFFIX = "_gl.vert";
constexpr const char* GL_FRAGMENT_SUFFIX = "_gl.frag";
constexpr const char* ENGINE_GBUFFER_FRAGMENT_STEM = "gbuffer";
constexpr const char* ENGINE_SHADOW_FRAGMENT_STEM = "shadow_depth";
constexpr const char* SHADOW_DEPTH_VERTEX =
    "assets/shaders/shadow_depth_gl.vert";
constexpr const char* SHADOW_POSITION_VERTEX =
    "assets/shaders/shadow_position_gl.vert";
constexpr const char* SHADOW_DEPTH_FRAGMENT =
    "assets/shaders/shadow_depth_gl.frag";
constexpr const char* LIGHTING_VERTEX = "assets/shaders/lighting_pass_gl.vert";
constexpr const char* FORMAT_PASS_NAMES[] = {"GBuffer", "shadow",
                                             "transparent"};

/// Path of a stem's OpenGL stage file.
std::string GLShaderPath(const std::string& stem, const char* suffix) {
    return SHADER_DIR + stem + suffix;
}

/// Sets one attribute's format on vao and routes it to the shared binding.
void SetAttribFormat(GLuint vao, GLuint location, GLint components, GLenum type,
                     bool integer, size_t offset) {
    const GLuint relativeOffset = static_cast<GLuint>(offset);
    if (integer)
        glVertexArrayAttribIFormat(vao, location, components, type,
                                   relativeOffset);
    else
        glVertexArrayAttribFormat(vao, location, components, type, GL_FALSE,
                                  relativeOffset);
    glVertexArrayAttribBinding(vao, location, VERTEX_BUFFER_BINDING);
}

}  // namespace

/// Registers this backend's buffer/shader/texture factories with ResourceManager.
OpenGLRenderer::OpenGLRenderer(Window* window)
    : m_Window(window) {
    this->Type = RendererType::OpenGL;

    ResourceManager::RegisterCreateBuffer(
        this, &OpenGLRenderer::CreateBuffer);
    ResourceManager::RegisterCreateShader(
        this, &OpenGLRenderer::CreateShader);
    ResourceManager::RegisterCreateTexture(
        this, &OpenGLRenderer::CreateTexture);
    ResourceManager::RegisterCreateCubemapTexture(
        this, &OpenGLRenderer::CreateCubemapTexture);
    ResourceManager::RegisterCreateCubemapTextureFromPanorama(
        this, &OpenGLRenderer::CreateCubemapTextureFromPanorama);
    ResourceManager::RegisterCreateTextureFromMemory(
        [](const void* data, uint32_t w, uint32_t h, TextureFormat fmt, uint32_t) -> Texture* {
            auto* tex = new OpenGLTexture();
            if (tex->LoadFromMemory(data, w, h, fmt)) return tex;
            delete tex;
            return nullptr;
        });
    ResourceManager::RegisterCreateTextureFromDesc(
        [](const TextureDesc& desc, const TextureMipLevel* levels,
           uint32_t levelCount) -> Texture* {
            auto* tex = new OpenGLTexture();
            if (tex->LoadMipChain(desc, levels, levelCount)) return tex;
            delete tex;
            return nullptr;
        });
}

OpenGLRenderer::~OpenGLRenderer() {
    Cleanup();
}

bool OpenGLRenderer::Initialize() {
    if (m_Initialized) {
        return true;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    m_GLContext = SDL_GL_CreateContext(m_Window->GetSDLWindow());
    if (!m_GLContext) {
        SDL_Log("Failed to create OpenGL context: %s", SDL_GetError());
        return false;
    }

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        SDL_Log("Failed to initialize Glad (OpenGL loader)");
        return false;
    }

    int width, height;
    SDL_GetWindowSize(m_Window->GetSDLWindow(), &width, &height);
    ApplyViewport(0, 0, width, height);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Apply initial VSync setting
    SDL_GL_SetSwapInterval(m_vsync ? 1 : 0);

    // Query max MSAA samples
    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    m_maxMsaaSampleCount = 1;
    if (maxSamples >= 8) m_maxMsaaSampleCount = 8;
    else if (maxSamples >= 4) m_maxMsaaSampleCount = 4;
    else if (maxSamples >= 2) m_maxMsaaSampleCount = 2;
    SLEAK_INFO("OpenGL max MSAA samples: {}", m_maxMsaaSampleCount);

    SetupVertexLayout();

    m_Initialized = true;
    SetPerformanceCounter(true);

    // Create GBuffer resources (deferred rendering)
    if (m_deferredEnabled) {
        CreateGBufferResources();
    }

    SLEAK_INFO("OpenGL renderer has been initialized successfully!");
    SLEAK_INFO("OpenGL Version: {}",
               (const char*)glGetString(GL_VERSION));

    return true;
}

void OpenGLRenderer::SetupVertexLayout() {
    glCreateVertexArrays(1, &m_VAO);
    m_configuredLayout = NO_CONFIGURED_LAYOUT;
    m_configuredLayoutDesc = nullptr;
    m_enabledAttribMask = 0;
}

void OpenGLRenderer::ConfigureVertexLayout(uint32_t layout,
                                           const VertexLayoutDesc* desc) {
    if (layout == m_configuredLayout) return;
    m_configuredLayout = layout;
    m_configuredLayoutDesc = desc;

    uint32_t enabledMask = 0;
    if (desc) {
        for (const auto& attr : desc->attributes) {
            switch (attr.format) {
                case VertexAttribFormat::Float1:
                    SetAttribFormat(m_VAO, attr.location, 1, GL_FLOAT, false,
                                    attr.offset);
                    break;
                case VertexAttribFormat::Float2:
                    SetAttribFormat(m_VAO, attr.location, 2, GL_FLOAT, false,
                                    attr.offset);
                    break;
                case VertexAttribFormat::Float3:
                    SetAttribFormat(m_VAO, attr.location, 3, GL_FLOAT, false,
                                    attr.offset);
                    break;
                case VertexAttribFormat::Float4:
                    SetAttribFormat(m_VAO, attr.location, 4, GL_FLOAT, false,
                                    attr.offset);
                    break;
                case VertexAttribFormat::UInt1:
                    SetAttribFormat(m_VAO, attr.location, 1, GL_UNSIGNED_INT,
                                    true, attr.offset);
                    break;
                case VertexAttribFormat::Int1:
                    SetAttribFormat(m_VAO, attr.location, 1, GL_INT, true,
                                    attr.offset);
                    break;
            }
            if (attr.location < ATTRIB_MASK_BITS)
                enabledMask |= (1u << attr.location);
            else
                glEnableVertexArrayAttrib(m_VAO, attr.location);
        }
    } else {
        // Standard 96-byte Vertex layout: 7 attributes
        SetAttribFormat(m_VAO, ATTRIB_POSITION, 3, GL_FLOAT, false,
                        offsetof(Vertex, px));
        SetAttribFormat(m_VAO, ATTRIB_NORMAL, 3, GL_FLOAT, false,
                        offsetof(Vertex, nx));
        SetAttribFormat(m_VAO, ATTRIB_TANGENT, 4, GL_FLOAT, false,
                        offsetof(Vertex, tx));
        SetAttribFormat(m_VAO, ATTRIB_COLOR, 4, GL_FLOAT, false,
                        offsetof(Vertex, r));
        SetAttribFormat(m_VAO, ATTRIB_UV, 2, GL_FLOAT, false,
                        offsetof(Vertex, u));
        SetAttribFormat(m_VAO, ATTRIB_BONE_IDS, 4, GL_INT, true,
                        offsetof(Vertex, boneIDs));
        SetAttribFormat(m_VAO, ATTRIB_BONE_WEIGHTS, 4, GL_FLOAT, false,
                        offsetof(Vertex, boneWeights));
        for (GLuint loc = ATTRIB_POSITION; loc <= ATTRIB_BONE_WEIGHTS; ++loc)
            enabledMask |= (1u << loc);
    }

    // Toggle only the slots whose enable state differs from the last layout
    const uint32_t changed = enabledMask ^ m_enabledAttribMask;
    for (uint32_t loc = 0; loc < ATTRIB_MASK_BITS; ++loc) {
        const uint32_t bit = 1u << loc;
        if (!(changed & bit)) continue;
        if (enabledMask & bit)
            glEnableVertexArrayAttrib(m_VAO, loc);
        else
            glDisableVertexArrayAttrib(m_VAO, loc);
    }
    m_enabledAttribMask = enabledMask;
}

void OpenGLRenderer::BeginRender() {
    m_sceneReadPrepared = false;
    if (m_vsyncChangeRequested)
        ApplyVSyncChange();
    if (m_msaaChangeRequested)
        ApplyMSAAChange();
    if (m_gbufferFormatChangeRequested) ApplyGBufferFormatChange();
    if (m_lightingShaderDirty) ApplyLightingShaderChange();

    // Commit staged lightVP before shadow pass so shadow + main agree.
    if (m_hasPendingLightVP) {
        memcpy(m_lightVP, m_pendingLightVP, sizeof(m_lightVP));
    }

    // Shadow pass before main rendering
    if (m_shadowPassEnabled) {
        RenderShadowPass();
    }

    if (m_gbufferCreated && m_deferredEnabled) {
        // Deferred path: geometry pass writes to GBuffer
        glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);
        glDrawBuffers(GBUFFER_COLOR_COUNT, GBUFFER_DRAW_BUFFERS);
        // sRGB RT0 encodes linear writes; off again at the lighting pass
        if (m_gbufferAlbedoSRGB) glEnable(GL_FRAMEBUFFER_SRGB);
        // glClear honours color masks: RT3 must take the 0 clear
        SetMotionModelWrites(true);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        SetMotionModelWrites(false);
        // Geometry pass must NOT blend — each pixel writes final GBuffer values
        glDisable(GL_BLEND);
        m_inGeometryPass = true;
        ResetFormatPassState();
    } else {
        // Forward path: bind MSAA FBO if active, else default FBO
        if (m_msaaFBO != 0)
            glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);
        else
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glClearColor(0.39f, 0.58f, 0.93f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    glBindVertexArray(m_VAO);

    if (bImInitialized) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }
}

void OpenGLRenderer::EndRender() {
    glDisable(GL_FRAMEBUFFER_SRGB);
    // Resolve MSAA FBO to default framebuffer before ImGui.
    // Skip in deferred mode — we rendered directly to FBO 0.
    if (m_msaaFBO != 0 && !IsDeferredEnabled()) {
        int width, height;
        SDL_GetWindowSize(m_Window->GetSDLWindow(), &width, &height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, width, height,
                          0, 0, width, height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // A captured frame leaves the UI out, as on Vulkan
    const bool capture = !m_capturePath.empty();
    if (bImInitialized) {
        ImGui::Render();
        if (!capture) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    if (capture) WriteFrameCapture();

    glBindVertexArray(0);
    SDL_GL_SwapWindow(m_Window->GetSDLWindow());

    UpdateFrameMetrics();
}

bool OpenGLRenderer::RequestFrameCapture(const std::string& path) {
    if (path.empty()) return false;
    m_capturePath = path;
    return true;
}

void OpenGLRenderer::WriteFrameCapture() {
    std::string path;
    path.swap(m_capturePath);
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(m_Window->GetSDLWindow(), &width, &height);
    if (width <= 0 || height <= 0) {
        SLEAK_ERROR("Frame capture: window has no pixels, {} not written",
                    path);
        return;
    }

    const size_t rowBytes = static_cast<size_t>(width) * FRAME_CAPTURE_CHANNELS;
    std::vector<uint8_t> bottomUp(rowBytes * static_cast<size_t>(height));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, CAPTURE_PACK_ALIGNMENT);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE,
                 bottomUp.data());
    glPixelStorei(GL_PACK_ALIGNMENT, DEFAULT_PACK_ALIGNMENT);

    // GL rows run bottom-up; the PPM starts at the top row
    std::vector<uint8_t> rgb(bottomUp.size());
    for (int y = 0; y < height; ++y) {
        const size_t src = static_cast<size_t>(height - 1 - y) * rowBytes;
        std::memcpy(rgb.data() + static_cast<size_t>(y) * rowBytes,
                    bottomUp.data() + src, rowBytes);
    }
    WriteFramePPM(path, static_cast<uint32_t>(width),
                  static_cast<uint32_t>(height), rgb);
}

void OpenGLRenderer::Cleanup() {
    if (m_Initialized) {
        CleanupSceneRead();
        CleanupGBufferResources();
        CleanupFrameUBOs();
        DestroyFormatPrograms();
        delete m_shadowDepthShader;
        m_shadowDepthShader = nullptr;
        delete m_shadowPositionShader;
        m_shadowPositionShader = nullptr;
        m_shadowProgramsCreated = false;
        if (m_shadowTransformUBO) { glDeleteBuffers(1, &m_shadowTransformUBO); m_shadowTransformUBO = 0; }
        if (m_shadowUBO) { glDeleteBuffers(1, &m_shadowUBO); m_shadowUBO = 0; }
        if (m_shadowDepthTex) { glDeleteTextures(1, &m_shadowDepthTex); m_shadowDepthTex = 0; }
        if (m_shadowFBO) { glDeleteFramebuffers(1, &m_shadowFBO); m_shadowFBO = 0; }
        m_shadowMapCreated = false;
        m_shadowMapRendered = false;
        m_shadowUBOCreated = false;
        CleanupMSAAFramebuffer();
        if (bImInitialized) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            bImInitialized = false;
        }
        if (m_VAO != 0) {
            glDeleteVertexArrays(1, &m_VAO);
            m_VAO = 0;
        }
        if (m_GLContext) {
            SDL_GL_DestroyContext(m_GLContext);
            m_GLContext = nullptr;
        }
        m_Initialized = false;
    }
}

#ifdef SLEAK_HOT_RELOAD
bool OpenGLRenderer::ReloadShaders(const std::vector<std::string>& stems) {
    for (OpenGLShader* shader : OpenGLShader::GetLiveShaders()) {
        const bool changed =
            std::ranges::any_of(stems, [shader](const std::string& stem) {
                return shader->UsesStem(stem);
            });
        if (!changed) continue;
        if (shader->Reload()) {
            SLEAK_INFO("Reloaded OpenGL shader '{}'", shader->GetStem());
        } else {
            SLEAK_WARN("Shader '{}' failed to reload, keeping the old program",
                       shader->GetStem());
        }
    }
    for (const std::string& stem : stems) {
        const bool used =
            std::ranges::any_of(OpenGLShader::GetLiveShaders(),
                                [&stem](const OpenGLShader* shader) {
                                    return shader->UsesStem(stem);
                                });
        if (!used)
            SLEAK_WARN("Hot reload: no loaded OpenGL shader is built from '{}'",
                       stem);
    }
    // Format programs and a lighting stem that failed get another try
    for (FormatPrograms& entry : m_formatPrograms) {
        for (size_t pass = 0; pass < FORMAT_PASS_COUNT; ++pass) {
            if (!entry.failed[pass]) continue;
            entry.failed[pass] = false;
            entry.resolved[pass] = false;
        }
    }
    if (!m_lightingShaderStem.empty() && !m_lightingOverrideShader) {
        m_lightingOverrideFailedStem.clear();
        m_lightingShaderDirty = true;
    }
    return true;
}
#endif

void OpenGLRenderer::Resize(uint32_t width, uint32_t height) {
    ApplyViewport(0, 0, static_cast<GLsizei>(width),
                  static_cast<GLsizei>(height));
    // Recreate MSAA FBO at new size
    if (m_msaaFBO != 0) {
        CleanupMSAAFramebuffer();
        CreateMSAAFramebuffer();
    }
    // Resize GBuffer textures
    if (m_gbufferCreated) {
        RecreateGBufferOnResize(static_cast<int>(width), static_cast<int>(height));
    }
}

void OpenGLRenderer::Draw(uint32_t vertexCount) { Draw(vertexCount, 0); }

void OpenGLRenderer::Draw(uint32_t vertexCount, uint32_t firstVertex) {
    if (m_formatDrawsSuppressed) return;
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawArrays(mode, static_cast<GLint>(firstVertex), vertexCount);
    DrawnVertices += vertexCount;
    DrawnTriangles += vertexCount / 3;
}

void OpenGLRenderer::DrawIndexed(uint32_t indexCount) {
    DrawIndexed(indexCount, 0, 0);
}

void OpenGLRenderer::DrawIndexed(uint32_t indexCount, uint32_t firstIndex,
                                 int32_t baseVertex) {
    if (m_formatDrawsSuppressed) return;
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    const void* offset = reinterpret_cast<const void*>(
        static_cast<uintptr_t>(firstIndex) * sizeof(uint32_t));
    glDrawElementsBaseVertex(mode, indexCount, GL_UNSIGNED_INT, offset,
                             baseVertex);
    DrawnVertices += indexCount;
    DrawnTriangles += indexCount / 3;
}

void OpenGLRenderer::DrawInstance(uint32_t instanceCount,
                                   uint32_t vertexPerInstance) {
    if (m_formatDrawsSuppressed) return;
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawArraysInstanced(mode, 0, vertexPerInstance,
                          instanceCount);
    DrawnVertices += vertexPerInstance * instanceCount;
    DrawnTriangles += (vertexPerInstance / 3) * instanceCount;
}

void OpenGLRenderer::DrawIndexedInstance(uint32_t instanceCount,
                                          uint32_t indexPerInstance) {
    DrawIndexedInstance(instanceCount, indexPerInstance, 0, 0);
}

void OpenGLRenderer::DrawIndexedInstance(uint32_t instanceCount,
                                         uint32_t indexPerInstance,
                                         uint32_t firstIndex,
                                         int32_t baseVertex) {
    if (m_formatDrawsSuppressed) return;
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    const void* offset = reinterpret_cast<const void*>(
        static_cast<uintptr_t>(firstIndex) * sizeof(uint32_t));
    glDrawElementsInstancedBaseVertex(mode, indexPerInstance, GL_UNSIGNED_INT,
                                      offset, instanceCount, baseVertex);
    DrawnVertices += indexPerInstance * instanceCount;
    DrawnTriangles += (indexPerInstance / 3) * instanceCount;
}

void OpenGLRenderer::BeginDebugLinePass() {
    m_debugLineMode = true;
}

void OpenGLRenderer::EndDebugLinePass() {
    m_debugLineMode = false;
}

void OpenGLRenderer::SetRenderFace(RenderFace face) {
    Face = face;
    ConfigureRenderFace();
}

void OpenGLRenderer::SetRenderMode(RenderMode mode) {
    Mode = mode;
    ConfigureRenderMode();
}

void OpenGLRenderer::SetViewport(float x, float y, float width,
                                  float height, float minDepth,
                                  float maxDepth) {
    ApplyViewport(static_cast<GLint>(x), static_cast<GLint>(y),
                  static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDepthRangef(minDepth, maxDepth);
}

void OpenGLRenderer::ApplyViewport(GLint x, GLint y, GLsizei width,
                                   GLsizei height) {
    glViewport(x, y, width, height);
    // GL rejects negative sizes and keeps the previous viewport
    if (width >= 0 && height >= 0) m_viewport = {x, y, width, height};
}

void OpenGLRenderer::ClearRenderTarget(float r, float g, float b,
                                        float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void OpenGLRenderer::ClearDepthStencil(bool clearDepth, bool clearStencil,
                                        float depth, uint8_t stencil) {
    GLbitfield mask = 0;
    if (clearDepth) {
        glClearDepthf(depth);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (clearStencil) {
        glClearStencil(stencil);
        mask |= GL_STENCIL_BUFFER_BIT;
    }
    if (mask) glClear(mask);
}

void OpenGLRenderer::SetDepthWrite(bool enabled) {
    glDepthMask(enabled ? GL_TRUE : GL_FALSE);
}

void OpenGLRenderer::SetDepthCompare(DepthCompare compare) {
    GLenum glFunc = GL_LESS;
    switch (compare) {
        case DepthCompare::Less:         glFunc = GL_LESS; break;
        case DepthCompare::LessEqual:    glFunc = GL_LEQUAL; break;
        case DepthCompare::Greater:      glFunc = GL_GREATER; break;
        case DepthCompare::GreaterEqual: glFunc = GL_GEQUAL; break;
        case DepthCompare::Equal:        glFunc = GL_EQUAL; break;
        case DepthCompare::NotEqual:     glFunc = GL_NOTEQUAL; break;
        case DepthCompare::Always:       glFunc = GL_ALWAYS; break;
        case DepthCompare::Never:        glFunc = GL_NEVER; break;
    }
    glDepthFunc(glFunc);
}

void OpenGLRenderer::SetCullEnabled(bool enabled) {
    if (enabled)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
}

void OpenGLRenderer::BindTexture(RefPtr<Sleak::Texture> texture, uint32_t slot) {
    if (texture.IsValid())
        texture->Bind(slot);
}

void OpenGLRenderer::BindVertexBuffer(RefPtr<BufferBase> buffer,
                                       uint32_t slot) {
    auto* glBuf = dynamic_cast<OpenGLBuffer*>(buffer.get());
    if (!glBuf) return;

    // A registered layout drives the attribute formats; 0 means default Vertex
    const uint32_t format = buffer->GetVertexFormat();
    const VertexLayoutDesc* desc = m_configuredLayoutDesc;
    if (format != m_configuredLayout) {
        desc = format != 0 ? VertexFormatRegistry::Get(format) : nullptr;
        ConfigureVertexLayout(desc ? format : DEFAULT_VERTEX_LAYOUT, desc);
    }

    if (desc && desc->transparentReadsScene && m_inForwardTransparentPass)
        PrepareSceneRead();
    SelectFormatProgram(desc ? format : DEFAULT_VERTEX_LAYOUT);

    const GLsizei stride = desc ? static_cast<GLsizei>(desc->stride)
                                : static_cast<GLsizei>(sizeof(Vertex));
    glVertexArrayVertexBuffer(m_VAO, VERTEX_BUFFER_BINDING,
                              glBuf->GetGLBuffer(), 0, stride);
}

void OpenGLRenderer::BindIndexBuffer(RefPtr<BufferBase> buffer,
                                      uint32_t slot) {
    auto* glBuf = dynamic_cast<OpenGLBuffer*>(buffer.get());
    if (!glBuf) return;

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, glBuf->GetGLBuffer());
}

void OpenGLRenderer::BindConstantBuffer(RefPtr<BufferBase> buffer,
                                         uint32_t slot) {
    auto* glBuf = dynamic_cast<OpenGLBuffer*>(buffer.get());
    if (!glBuf) return;

    // Shadow pass: use dedicated shadow CB with LightVP*World for slot 0
    if (m_inShadowPass && slot == 0 &&
        glBuf->GetCPUShadowCopySize() >= TRANSFORM_CB_BYTES) {
        const float* srcWorld = reinterpret_cast<const float*>(
            static_cast<const char*>(glBuf->GetCPUShadowCopy()) + MAT4_BYTES);

        // Create dedicated shadow transform UBO on first use
        if (m_shadowTransformUBO == 0) {
            glCreateBuffers(1, &m_shadowTransformUBO);
            glNamedBufferData(m_shadowTransformUBO, TRANSFORM_CB_BYTES, nullptr,
                              GL_DYNAMIC_DRAW);
            m_shadowTransformCacheValid = false;
        }

        // LightVP is fixed for the pass, so only a new World needs an upload
        static_assert(sizeof(m_shadowTransformCache) == TRANSFORM_CB_BYTES);
        static_assert(sizeof(m_shadowWorldCache) == MAT4_BYTES);
        if (!m_shadowTransformCacheValid ||
            memcmp(srcWorld, m_shadowWorldCache, MAT4_BYTES) != 0) {
            for (int r = 0; r < MAT4_DIM; ++r) {
                for (int c = 0; c < MAT4_DIM; ++c) {
                    float sum = 0.0f;
                    for (int k = 0; k < MAT4_DIM; ++k) {
                        sum += srcWorld[r * MAT4_DIM + k] *
                               m_lightVP[k * MAT4_DIM + c];
                    }
                    m_shadowTransformCache[r * MAT4_DIM + c] = sum;
                }
            }
            memcpy(&m_shadowTransformCache[MAT4_FLOATS], srcWorld, MAT4_BYTES);
            memcpy(m_shadowWorldCache, srcWorld, MAT4_BYTES);
            m_shadowTransformCacheValid = true;
            glNamedBufferSubData(m_shadowTransformUBO, 0,
                                 sizeof(m_shadowTransformCache),
                                 m_shadowTransformCache);
        }

        glBindBufferBase(GL_UNIFORM_BUFFER, slot, m_shadowTransformUBO);
        return;
    }

    glBindBufferBase(GL_UNIFORM_BUFFER, slot, glBuf->GetGLBuffer());
}

void OpenGLRenderer::BindBoneBuffer(RefPtr<BufferBase> buffer) {
    BindConstantBuffer(buffer, 3);
}

BufferBase* OpenGLRenderer::CreateBuffer(BufferType type, uint32_t size,
                                          void* data) {
    auto* buffer = new OpenGLBuffer(size, type);
    if (!buffer->Initialize(data)) {
        delete buffer;
        return nullptr;
    }
    return buffer;
}

Shader* OpenGLRenderer::CreateShader(const std::string& shaderSource) {
    auto* shader = new OpenGLShader();
    if (shader->compile(shaderSource)) {
        return shader;
    }
    delete shader;
    return nullptr;
}

Texture* OpenGLRenderer::CreateTexture(const std::string& TexturePath) {
    auto* texture = new OpenGLTexture();
    if (texture->LoadFromFile(TexturePath)) {
        return texture;
    }
    delete texture;
    return nullptr;
}

Texture* OpenGLRenderer::CreateTextureFromData(uint32_t width,
                                                uint32_t height,
                                                void* data) {
    auto* texture = new OpenGLTexture();
    if (texture->LoadFromMemory(data, width, height,
                                 TextureFormat::RGBA8)) {
        return texture;
    }
    delete texture;
    return nullptr;
}

void OpenGLRenderer::CreateMSAAFramebuffer() {
    if (m_msaaSampleCount <= 1)
        return;

    int width, height;
    SDL_GetWindowSize(m_Window->GetSDLWindow(), &width, &height);

    glGenFramebuffers(1, &m_msaaFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);

    // Multisampled color renderbuffer
    glGenRenderbuffers(1, &m_msaaColorRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_msaaColorRBO);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_msaaSampleCount,
                                     GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_RENDERBUFFER, m_msaaColorRBO);

    // Multisampled depth renderbuffer
    glGenRenderbuffers(1, &m_msaaDepthRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_msaaDepthRBO);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_msaaSampleCount,
                                     GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_RENDERBUFFER, m_msaaDepthRBO);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SLEAK_ERROR("OpenGL MSAA framebuffer is not complete!");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLRenderer::CleanupMSAAFramebuffer() {
    if (m_msaaDepthRBO) { glDeleteRenderbuffers(1, &m_msaaDepthRBO); m_msaaDepthRBO = 0; }
    if (m_msaaColorRBO) { glDeleteRenderbuffers(1, &m_msaaColorRBO); m_msaaColorRBO = 0; }
    if (m_msaaFBO) { glDeleteFramebuffers(1, &m_msaaFBO); m_msaaFBO = 0; }
}

void OpenGLRenderer::ApplyMSAAChange() {
    if (!m_msaaChangeRequested)
        return;
    m_msaaChangeRequested = false;
    m_msaaSampleCount = m_pendingMsaaSampleCount;

    CleanupMSAAFramebuffer();
    CreateMSAAFramebuffer();

    SLEAK_INFO("OpenGL MSAA changed to {}x", m_msaaSampleCount);
}

void OpenGLRenderer::ApplyVSyncChange() {
    if (!m_vsyncChangeRequested)
        return;
    m_vsyncChangeRequested = false;
    SDL_GL_SetSwapInterval(m_vsync ? 1 : 0);
    SLEAK_INFO("OpenGL VSync {}", m_vsync ? "enabled" : "disabled");
}

void OpenGLRenderer::ConfigureRenderMode() {
    switch (Mode) {
        case RenderMode::Fill:
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            break;
        case RenderMode::Wireframe:
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            break;
        case RenderMode::Points:
            glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
            break;
        default:
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            break;
    }
}

void OpenGLRenderer::ConfigureRenderFace() {
    switch (Face) {
        case RenderFace::None:
            glDisable(GL_CULL_FACE);
            break;
        case RenderFace::Back:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            break;
        case RenderFace::Front:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            break;
        default:
            glDisable(GL_CULL_FACE);
            break;
    }
}

Texture* OpenGLRenderer::CreateCubemapTexture(const std::array<std::string, 6>& facePaths) {
    auto* texture = new OpenGLCubemapTexture();
    if (texture->LoadCubemap(facePaths)) {
        return texture;
    }
    delete texture;
    return nullptr;
}

Texture* OpenGLRenderer::CreateCubemapTextureFromPanorama(const std::string& panoramaPath) {
    auto* texture = new OpenGLCubemapTexture();
    if (texture->LoadEquirectangular(panoramaPath)) {
        return texture;
    }
    delete texture;
    return nullptr;
}

bool OpenGLRenderer::CreateImGUI() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForOpenGL(m_Window->GetSDLWindow(),
                                       m_GLContext))
        return false;

    if (!ImGui_ImplOpenGL3_Init("#version 450"))
        return false;

    bImInitialized = true;
    return true;
}

void OpenGLRenderer::SetLightVP(const float* mat) {
    if (mat) {
        memcpy(m_pendingLightVP, mat, sizeof(m_pendingLightVP));
        m_hasPendingLightVP = true;
    }
}

bool OpenGLRenderer::CreateShadowUBO() {
    if (m_shadowUBOCreated) return true;
    glGenBuffers(1, &m_shadowUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_shadowUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(RenderEngine::PCSSShadowGPUData), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    m_shadowUBOCreated = true;
    return true;
}

void OpenGLRenderer::UpdateShadowLightUBO(const void* data, uint32_t size) {
    if (!m_shadowUBOCreated && !CreateShadowUBO()) return;
    if (!data) return;

    const auto* ubo = static_cast<const RenderEngine::ShadowLightUBO*>(data);
    if (size >= sizeof(RenderEngine::ShadowLightUBO)) {
        m_frameData.SetLight(*ubo);
        if (CreateFrameUBOs()) {
            RenderEngine::ShadowLightUBO raw = *ubo;
            if (m_shadowMapRendered)
                memcpy(raw.LightVP, m_shadowMapLightVP, sizeof(raw.LightVP));
            glNamedBufferSubData(m_shadowLightRawUBO, 0, sizeof(raw), &raw);
            glBindBufferBase(GL_UNIFORM_BUFFER, SHADOW_LIGHT_UBO_BINDING,
                             m_shadowLightRawUBO);
        }
    }

    RenderEngine::PCSSShadowGPUData shadowData{};
    // Sample with the map's own matrix
    memcpy(shadowData.LightVP,
           m_shadowMapRendered ? m_shadowMapLightVP : ubo->LightVP,
           sizeof(shadowData.LightVP));
    memcpy(shadowData.NdcToShadow, ubo->NdcToShadow, sizeof(float) * 16);
    shadowData.ShadowBias = ubo->ShadowBias;
    shadowData.ShadowStrength = ubo->ShadowStrength;
    shadowData.ShadowTexelSize = ubo->ShadowTexelSize;
    shadowData.ShadowLightSize = ubo->LightSize;
    shadowData.PCSSEnabled = m_pcssEnabled ? 1 : 0;
    shadowData.ShadowMapEnabled = m_shadowPassEnabled ? 1 : 0;

    glBindBuffer(GL_UNIFORM_BUFFER, m_shadowUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(shadowData), &shadowData);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // Bind shadow UBO at binding 5 (matches shader)
    glBindBufferBase(GL_UNIFORM_BUFFER, 5, m_shadowUBO);
}

bool OpenGLRenderer::CreateFrameUBOs() {
    if (m_frameUBOsCreated) return true;
    struct FrameBuffer {
        GLuint* buffer;
        size_t size;
        GLuint binding;
    };
    const FrameBuffer buffers[] = {
        {&m_frameDataUBO, sizeof(RenderEngine::FrameData),
         FRAME_DATA_UBO_BINDING},
        {&m_frameUserUBO, RenderEngine::FRAME_USER_DATA_MAX_BYTES,
         FRAME_USER_UBO_BINDING},
        {&m_frameCascadeUBO, RenderEngine::FRAME_CASCADE_BLOCK_BYTES,
         FRAME_CASCADE_UBO_BINDING},
        {&m_shadowLightRawUBO, sizeof(RenderEngine::ShadowLightUBO),
         SHADOW_LIGHT_UBO_BINDING},
        {&m_frameAtmosphereUBO, RenderEngine::FRAME_ATMOSPHERE_BLOCK_BYTES,
         FRAME_ATMOSPHERE_UBO_BINDING},
    };
    for (const FrameBuffer& entry : buffers) {
        const std::vector<uint8_t> zeros(entry.size, 0);
        glCreateBuffers(1, entry.buffer);
        glNamedBufferData(*entry.buffer, static_cast<GLsizeiptr>(entry.size),
                          zeros.data(), GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, entry.binding, *entry.buffer);
    }
    m_frameUBOsCreated = true;
    return true;
}

void OpenGLRenderer::CleanupFrameUBOs() {
    for (GLuint* buffer : {&m_frameDataUBO, &m_frameUserUBO, &m_frameCascadeUBO,
                           &m_shadowLightRawUBO, &m_frameAtmosphereUBO}) {
        if (*buffer) glDeleteBuffers(1, buffer);
        *buffer = 0;
    }
    m_frameUBOsCreated = false;
}

void OpenGLRenderer::UploadFrameData() {
    if (!CreateFrameUBOs()) return;
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(m_Window->GetSDLWindow(), &width, &height);

    RenderEngine::FrameTargetInfo target;
    target.width = static_cast<uint32_t>(std::max(width, 0));
    target.height = static_cast<uint32_t>(std::max(height, 0));
    if (m_ssaoEnabled && m_ssaoCreated && IsDeferredEnabled())
        target.featureFlags |= RenderEngine::FrameFlagSSAO;
    if (m_shadowPassEnabled)
        target.featureFlags |= RenderEngine::FrameFlagShadows;
    if (IsDeferredEnabled())
        target.featureFlags |= RenderEngine::FrameFlagDeferred;

    RenderEngine::FrameData data{};
    target.output = GetOutputTransform();
    m_frameData.Build(target, data);
    glNamedBufferSubData(m_frameDataUBO, 0, sizeof(data), &data);
    glNamedBufferSubData(m_frameUserUBO, 0,
                         RenderEngine::FRAME_USER_DATA_MAX_BYTES,
                         m_frameData.GetUserData());
    glBindBufferBase(GL_UNIFORM_BUFFER, FRAME_DATA_UBO_BINDING, m_frameDataUBO);
    glBindBufferBase(GL_UNIFORM_BUFFER, FRAME_USER_UBO_BINDING, m_frameUserUBO);
}

bool OpenGLRenderer::CreateShadowMapResources() {
    if (m_shadowMapCreated) return true;

    // Create depth texture
    glGenTextures(1, &m_shadowDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
                 m_shadowMapResolution, m_shadowMapResolution, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create FBO
    glGenFramebuffers(1, &m_shadowFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowDepthTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SLEAK_ERROR("OpenGL shadow FBO is not complete!");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_shadowMapCreated = true;
    m_shadowResourcesCreated = true;
    SLEAK_INFO("OpenGL shadow map resources created ({}x{})", m_shadowMapResolution, m_shadowMapResolution);
    return true;
}

void OpenGLRenderer::RenderShadowPass() {
    // Skip if no cached draws — preserve previous frame's shadow map
    auto* queue = RenderCommandQueue::GetInstance();
    if (!queue || !queue->HasCachedShadowDraws()) return;

    if (!m_shadowMapCreated) {
        if (!CreateShadowMapResources()) return;
    }

    const std::array<GLint, 4> savedViewport = m_viewport;

    // Bind shadow FBO
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
    glViewport(0, 0, m_shadowMapResolution, m_shadowMapResolution);
    glClear(GL_DEPTH_BUFFER_BIT);

    // Enable polygon offset for slope-scale depth bias
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);

    // Back-face cull: closed solid volumes always have the light-facing
    // surface win — same depth result, half the raster.
    // (FRONT culling stays wrong: it removed the outward-facing surfaces.)
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Ensure VAO is bound for shadow pass draws
    glBindVertexArray(m_VAO);

    // Engine depth program for default-layout casters; formats swap theirs in
    CreateShadowPrograms();
    if (m_shadowDepthShader) m_shadowDepthShader->bind();

    // Execute shadow draw commands
    m_inShadowPass = true;
    m_shadowTransformCacheValid = false;
    ResetFormatPassState();
    if (queue) {
        queue->ExecuteShadowPass(this);
    }
    RestorePassCull();
    m_formatDrawsSuppressed = false;
    m_inShadowPass = false;
    memcpy(m_shadowMapLightVP, m_lightVP, sizeof(m_shadowMapLightVP));
    m_shadowMapRendered = true;

    // Restore state
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE); // re-enable culling for main pass
    ConfigureRenderFace();

    // Restore framebuffer
    if (m_msaaFBO != 0)
        glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);
    else
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ApplyViewport(savedViewport[0], savedViewport[1], savedViewport[2],
                  savedViewport[3]);

    // Bind shadow map texture at unit 3
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);
    glActiveTexture(GL_TEXTURE0);
}

// ============================================================
// Deferred rendering — GBuffer implementation
// ============================================================

bool OpenGLRenderer::IsDeferredEnabled() const {
    return m_deferredEnabled && m_gbufferCreated;
}

bool OpenGLRenderer::CreateGBufferResources() {
    if (m_gbufferCreated) return true;

    int w, h;
    SDL_GetWindowSize(m_Window->GetSDLWindow(), &w, &h);
    m_gbufferWidth  = w;
    m_gbufferHeight = h;

    // ---- GBuffer FBO ----
    glGenFramebuffers(1, &m_gbufferFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);

    auto makeColorTex = [](GLuint& tex, GLenum internalFmt, int w, int h) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        GLenum fmt  = GL_RGBA;
        GLenum type = GL_UNSIGNED_BYTE;
        if (internalFmt == GL_RGBA16F || internalFmt == GL_RGBA32F) {
            type = GL_FLOAT;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, fmt, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };

    // RT0: Albedo + AO   (RGBA8)
    makeColorTex(m_gbufferAlbedoAO, GBufferAlbedoFormat(), w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gbufferAlbedoAO, 0);

    // RT1: Normal + Roughness  (RGBA16F)
    makeColorTex(m_gbufferNormalRough, GL_RGBA16F, w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gbufferNormalRough, 0);

    // RT2: Metallic + Emissive  (RGBA8)
    makeColorTex(m_gbufferMetalEmit,   GL_RGBA8,   w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_gbufferMetalEmit, 0);

    // RT3: object motion, game channel, shading model  (RGBA16F)
    makeColorTex(m_gbufferMotionModel, GL_RGBA16F, w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D,
                           m_gbufferMotionModel, 0);

    // World position is reconstructed from depth + InvViewProj in the
    // lighting/SSAO passes (no RGBA32F worldpos RT) to cut GBuffer bandwidth.

    // Depth texture (read back in lighting pass for world pos reconstruction)
    glGenTextures(1, &m_gbufferDepth);
    glBindTexture(GL_TEXTURE_2D, m_gbufferDepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w, h, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_gbufferDepth, 0);

    glDrawBuffers(GBUFFER_COLOR_COUNT, GBUFFER_DRAW_BUFFERS);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SLEAK_ERROR("OpenGL GBuffer FBO is not complete!");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // ---- GBuffer geometry shader ----
    m_gbufferShader = new OpenGLShader();
    if (!m_gbufferShader->compile("assets/shaders/gbuffer_gl.vert",
                                   "assets/shaders/gbuffer_gl.frag")) {
        SLEAK_ERROR("Failed to compile GBuffer geometry shader!");
        delete m_gbufferShader;
        m_gbufferShader = nullptr;
        CleanupGBufferResources();
        return false;
    }

    // ---- Lighting pass shader ----
    m_lightingShader = new OpenGLShader();
    if (!m_lightingShader->compile("assets/shaders/lighting_pass_gl.vert",
                                    "assets/shaders/lighting_pass_gl.frag")) {
        SLEAK_ERROR("Failed to compile deferred lighting pass shader!");
        delete m_lightingShader;
        m_lightingShader = nullptr;
        CleanupGBufferResources();
        return false;
    }

    // ---- Empty VAO for fullscreen triangle ----
    glGenVertexArrays(1, &m_lightingVAO);

    // ---- Deferred CB UBO (binding 6) ----
    glGenBuffers(1, &m_deferredCBUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_deferredCBUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(RenderEngine::DeferredCBData), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 6, m_deferredCBUBO);
    m_deferredCBCreated = true;

    m_gbufferCreated = true;
    SLEAK_INFO("OpenGL GBuffer created ({}x{}) — deferred rendering active", w, h);
    // A lighting stem set before the GBuffer existed applies next frame
    if (!m_lightingShaderStem.empty()) m_lightingShaderDirty = true;

    if (!CreateSSAOResources()) {
        SLEAK_WARN("OpenGL SSAO resources failed to initialize — SSAO disabled");
    }
    return true;
}

void OpenGLRenderer::CleanupGBufferResources() {
    CleanupSSAOResources();
    CleanupIBL();
    if (m_gbufferAlbedoAO)    { glDeleteTextures(1, &m_gbufferAlbedoAO);    m_gbufferAlbedoAO    = 0; }
    if (m_gbufferNormalRough) { glDeleteTextures(1, &m_gbufferNormalRough); m_gbufferNormalRough = 0; }
    if (m_gbufferMetalEmit)   { glDeleteTextures(1, &m_gbufferMetalEmit);   m_gbufferMetalEmit   = 0; }
    if (m_gbufferMotionModel) {
        glDeleteTextures(1, &m_gbufferMotionModel);
        m_gbufferMotionModel = 0;
    }
    if (m_gbufferDepth)       { glDeleteTextures(1, &m_gbufferDepth);       m_gbufferDepth       = 0; }
    if (m_gbufferFBO)         { glDeleteFramebuffers(1, &m_gbufferFBO);     m_gbufferFBO         = 0; }
    if (m_lightingVAO)        { glDeleteVertexArrays(1, &m_lightingVAO);    m_lightingVAO        = 0; }
    if (m_deferredCBUBO)      { glDeleteBuffers(1, &m_deferredCBUBO);       m_deferredCBUBO      = 0; }
    delete m_gbufferShader;  m_gbufferShader  = nullptr;
    delete m_lightingShader; m_lightingShader = nullptr;
    delete m_lightingOverrideShader;
    m_lightingOverrideShader = nullptr;
    m_gbufferCreated    = false;
    m_deferredCBCreated = false;
}

// IBL — lazy bake from active scene's skybox cubemap.
// First call (or when the skybox cubemap changes) reruns the precompute.
void OpenGLRenderer::EnsureIBL() {
    GLuint cubeHandle = FindSkyboxCubemap();
    if (cubeHandle == 0) return;

    if (!m_ibl) m_ibl = new OpenGLIBL();
    if (cubeHandle != m_iblBoundCubemap) {
        m_ibl->Cleanup();
        if (m_ibl->Initialize(cubeHandle)) {
            m_iblBoundCubemap = cubeHandle;
        } else {
            m_iblBoundCubemap = 0;
        }
    }

    if (m_iblSettingsUBO == 0) {
        glGenBuffers(1, &m_iblSettingsUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, m_iblSettingsUBO);
        glBufferData(GL_UNIFORM_BUFFER,
                     sizeof(IBLSettingsGPUData),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
}

void OpenGLRenderer::CleanupIBL() {
    if (m_ibl) { delete m_ibl; m_ibl = nullptr; }
    if (m_iblSettingsUBO) {
        glDeleteBuffers(1, &m_iblSettingsUBO);
        m_iblSettingsUBO = 0;
    }
    m_iblBoundCubemap = 0;
}

GLenum OpenGLRenderer::GBufferAlbedoFormat() const {
    return m_gbufferAlbedoSRGB ? GBUFFER_ALBEDO_SRGB_FORMAT
                               : GBUFFER_ALBEDO_FORMAT;
}

void OpenGLRenderer::ApplyGBufferFormatChange() {
    m_gbufferFormatChangeRequested = false;
    // Not created yet: CreateGBufferResources picks the format up
    if (!m_gbufferCreated) return;
    glBindTexture(GL_TEXTURE_2D, m_gbufferAlbedoAO);
    glTexImage2D(GL_TEXTURE_2D, 0, GBufferAlbedoFormat(), m_gbufferWidth,
                 m_gbufferHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderer::RecreateGBufferOnResize(int width, int height) {
    if (!m_gbufferCreated) return;

    m_gbufferWidth  = width;
    m_gbufferHeight = height;

    // Resize AlbedoAO
    glBindTexture(GL_TEXTURE_2D, m_gbufferAlbedoAO);
    glTexImage2D(GL_TEXTURE_2D, 0, GBufferAlbedoFormat(), width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Resize NormalRough
    glBindTexture(GL_TEXTURE_2D, m_gbufferNormalRough);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);

    // Resize MetalEmit
    glBindTexture(GL_TEXTURE_2D, m_gbufferMetalEmit);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Resize MotionModel
    glBindTexture(GL_TEXTURE_2D, m_gbufferMotionModel);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
                 GL_FLOAT, nullptr);

    // Resize depth
    glBindTexture(GL_TEXTURE_2D, m_gbufferDepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);

    RecreateSSAOOnResize(width, height);

    SLEAK_INFO("OpenGL GBuffer resized to {}x{}", width, height);
}

void OpenGLRenderer::BindGBufferShader() {
    if (m_gbufferShader) {
        // Ensure GBuffer FBO is active with all 4 color attachments
        // (RT0=AlbedoAO, RT1=NormalRough, RT2=MetalEmit, RT3=MotionModel)
        glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);
        glDrawBuffers(GBUFFER_COLOR_COUNT, GBUFFER_DRAW_BUFFERS);
        SetMotionModelWrites(false);
        // Ensure depth writes are enabled for geometry pass
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);

        // Clear stale shadow sampler from unit 3 — the shadow depth texture
        // has GL_TEXTURE_COMPARE_MODE enabled which conflicts with the GBuffer
        // shader's sampler2D declaration at binding 3 (roughnessTexture).
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);

        m_gbufferShader->bind();
        glBindVertexArray(m_VAO);
    } else {
        static bool warned = false;
        if (!warned) {
            SLEAK_ERROR("BindGBufferShader: m_gbufferShader is NULL!");
            warned = true;
        }
    }
}

void OpenGLRenderer::BindPBRMaterial(Sleak::Material* material) {
    // Deferred geometry-pass material bind (restores pre-rewrite OpenGL path):
    // bind the GBuffer shader + the material textures/CB. Without this, terrain
    // is drawn with the leftover water program (all-blue bug).
    BindGBufferShader();
    if (material) material->BindTexturesAndCB();
}

void OpenGLRenderer::UpdateDeferredCB(const void* data, uint32_t size) {
    if (!m_deferredCBCreated) return;
    if (data && size >= sizeof(DeferredCBData)) {
        const auto* cb = static_cast<const DeferredCBData*>(data);
        memcpy(m_cachedInvViewProj, cb->InvViewProj,
               sizeof(m_cachedInvViewProj));
        m_cachedNearPlane = cb->NearPlane;
        m_cachedFarPlane = cb->FarPlane;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, m_deferredCBUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, size, data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 6, m_deferredCBUBO);
}

void OpenGLRenderer::ExecuteDeferredLightingPass() {
    // The default framebuffer never encodes: the shaders write display values
    glDisable(GL_FRAMEBUFFER_SRGB);
    // Geometry pass state back to its defaults
    RestorePassCull();
    SetMotionModelWrites(true);
    ResetFormatPassState();
    m_inGeometryPass = false;

    // The game lighting stem when one loaded
    OpenGLShader* lighting =
        m_lightingOverrideShader ? m_lightingOverrideShader : m_lightingShader;
    if (!m_gbufferCreated || !lighting) return;

    // SSAO must run BEFORE the lighting pass so its blurred output is
    // available for the lighting shader to multiply against vertex AO.
    ExecuteSSAOPass();

    // IBL precompute (cheap when already baked) — must happen before the
    // lighting shader binds units 14/15/16 and UBO 10.
    EnsureIBL();

    // Deferred rendering always outputs to the default FBO (framebuffer 0).
    // MSAA is incompatible with deferred (multisampled GBuffer is too expensive),
    // so we skip the MSAA FBO and render directly to the default framebuffer.
    constexpr GLuint targetFBO = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);

    // Keep the sky color cleared
    glClearColor(0.39f, 0.58f, 0.93f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Enable depth writes so the lighting shader can output gl_FragDepth.
    // This replaces the old glBlitFramebuffer approach which silently failed
    // on some drivers when GBuffer depth (32F) differs from default FBO depth (24-bit).
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);     // fullscreen triangle must always pass
    glDepthMask(GL_TRUE);       // allow depth writes
    glDisable(GL_BLEND);

    // Bind lighting shader
    lighting->bind();

    // Bind GBuffer textures at units 8-12
    glActiveTexture(GL_TEXTURE8);  glBindTexture(GL_TEXTURE_2D, m_gbufferAlbedoAO);
    glActiveTexture(GL_TEXTURE9);  glBindTexture(GL_TEXTURE_2D, m_gbufferNormalRough);
    glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_2D, m_gbufferMetalEmit);
    glActiveTexture(GL_TEXTURE11); glBindTexture(GL_TEXTURE_2D, m_gbufferDepth);
    glActiveTexture(GBUFFER_MOTION_MODEL_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, m_gbufferMotionModel);
    // SSAO result at unit 13 (default to 0 = "fully lit" if SSAO disabled)
    glActiveTexture(GL_TEXTURE13);
    glBindTexture(GL_TEXTURE_2D, m_ssaoCreated ? m_ssaoBlurTex : 0);
    // SSAO enable flag via plain uniform — avoids growing DeferredCBData
    GLint ssaoLoc =
        glGetUniformLocation(lighting->GetProgram(), "uSSAOEnabled");
    if (ssaoLoc >= 0) {
        glUniform1i(ssaoLoc, (m_ssaoEnabled && m_ssaoCreated) ? 1 : 0);
    }
    glActiveTexture(GL_TEXTURE0);

    // Rebind shadow map at unit 3 — BindGBufferShader cleared it to avoid
    // sampler2DShadow/sampler2D conflict during the geometry pass.
    if (m_shadowMapCreated) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);
        glActiveTexture(GL_TEXTURE0);
    }

    // IBL — bind irradiance/prefilter/BRDF at units 14/15/16 and upload the
    // settings UBO at slot 10. Disabled if precompute hasn't completed.
    bool iblReady = (m_ibl && m_ibl->IsInitialized());
    if (iblReady) m_ibl->Bind();
    if (m_iblSettingsUBO) {
        IBLSettingsGPUData ibl{};
        ibl.IBLEnabled       = (iblReady && m_iblEnabled) ? 1u : 0u;
        ibl.IBLIntensity     = m_iblIntensity;
        ibl.MaxReflectionLOD = static_cast<float>(OpenGLIBL::PREFILTER_MIPS - 1);
        ibl._iblPad0         = 0;
        glBindBuffer(GL_UNIFORM_BUFFER, m_iblSettingsUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ibl), &ibl);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        glBindBufferBase(GL_UNIFORM_BUFFER, 10, m_iblSettingsUBO);
    }

    // Draw fullscreen triangle — shader writes color + gl_FragDepth
    glBindVertexArray(m_lightingVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Restore depth state for forward pass
    glDepthFunc(GL_LESS);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Rebind main VAO for forward pass
    glBindVertexArray(m_VAO);
}

void OpenGLRenderer::BeginForwardTransparentPass() {
    m_inForwardTransparentPass = true;
    ResetFormatPassState();
    // Transparent objects need alpha blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void OpenGLRenderer::EndForwardTransparentPass() {
    m_inForwardTransparentPass = false;
    m_formatDrawsSuppressed = false;
}

/// Rebuilds or drops the lighting override after SetLightingShader.
void OpenGLRenderer::ApplyLightingShaderChange() {
    if (!m_gbufferCreated) return;  // CreateGBufferResources reapplies
    m_lightingShaderDirty = false;
    delete m_lightingOverrideShader;
    m_lightingOverrideShader = nullptr;
    if (m_lightingShaderStem.empty()) return;

    auto* shader = new OpenGLShader();
    if (shader->compile(LIGHTING_VERTEX, GLShaderPath(m_lightingShaderStem,
                                                      GL_FRAGMENT_SUFFIX))) {
        m_lightingOverrideShader = shader;
        m_lightingOverrideFailedStem.clear();
        SLEAK_INFO("OpenGL: lighting shader '{}' active", m_lightingShaderStem);
        return;
    }
    delete shader;
    if (m_lightingOverrideFailedStem == m_lightingShaderStem) return;
    SLEAK_ERROR(
        "OpenGL: lighting shader '{}' failed to load, keeping the engine "
        "lighting",
        m_lightingShaderStem);
    m_lightingOverrideFailedStem = m_lightingShaderStem;
}

// ============================================================
// Custom vertex format programs
// ============================================================

/// Compiles the engine shadow depth programs once.
void OpenGLRenderer::CreateShadowPrograms() {
    if (m_shadowProgramsCreated) return;
    m_shadowProgramsCreated = true;
    m_shadowDepthShader = new OpenGLShader();
    if (!m_shadowDepthShader->compile(SHADOW_DEPTH_VERTEX,
                                      SHADOW_DEPTH_FRAGMENT)) {
        SLEAK_ERROR("OpenGL: shadow depth program failed to build");
        delete m_shadowDepthShader;
        m_shadowDepthShader = nullptr;
    }
    m_shadowPositionShader = new OpenGLShader();
    if (!m_shadowPositionShader->compile(SHADOW_POSITION_VERTEX,
                                         SHADOW_DEPTH_FRAGMENT)) {
        SLEAK_ERROR("OpenGL: shadow position program failed to build");
        delete m_shadowPositionShader;
        m_shadowPositionShader = nullptr;
    }
}

/// Records the caster material for alpha-tested shadow programs.
void OpenGLRenderer::BindShadowMaterial(Sleak::Material* material) {
    m_shadowMaterial = material;
    m_shadowMaterialDirty = true;
    FlushShadowMaterial();
}

/// Binds the caster diffuse at unit 0 for a sampling shadow program.
void OpenGLRenderer::FlushShadowMaterial() {
    if (!m_inShadowPass || !m_shadowProgramSamples || !m_shadowMaterialDirty)
        return;
    Sleak::Texture* diffuse =
        m_shadowMaterial ? m_shadowMaterial->GetDiffuseTexture() : nullptr;
    if (diffuse)
        diffuse->Bind(SHADOW_MATERIAL_UNIT);
    else
        glBindTextureUnit(SHADOW_MATERIAL_UNIT, 0);
    m_shadowMaterialDirty = false;
}

/// Pass the next custom-format draw resolves a program for.
OpenGLRenderer::FormatPass OpenGLRenderer::CurrentFormatPass() const {
    if (m_inShadowPass) return FormatPass::Shadow;
    if (m_inGeometryPass) return FormatPass::GBuffer;
    if (m_inForwardTransparentPass) return FormatPass::Transparent;
    return FormatPass::Count;
}

/// A format's program for a pass, built on first use.
OpenGLShader* OpenGLRenderer::GetFormatProgram(VertexFormatHandle format,
                                               FormatPass pass, bool& skip) {
    skip = false;
    if (format > m_formatPrograms.size()) {
        if (!VertexFormatRegistry::Get(format)) return nullptr;
        m_formatPrograms.resize(format);
    }
    FormatPrograms& entry = m_formatPrograms[format - 1];
    const size_t slot = static_cast<size_t>(pass);
    if (entry.failed[slot]) {
        skip = true;
        return nullptr;
    }
    if (entry.resolved[slot]) return entry.programs[slot];
    entry.resolved[slot] = true;

    const VertexLayoutDesc* desc = VertexFormatRegistry::Get(format);
    if (!desc) return nullptr;
    entry.writesMotionModel = !desc->gbufferFragmentStem.empty();
    entry.shadowSamples = !desc->shadowFragmentStem.empty();
    entry.cullMode = desc->cullMode;
    entry.shadowCullMode = desc->shadowCullMode;

    std::string vertStem = desc->transparentShaderStem;
    std::string fragStem = desc->transparentShaderStem;
    if (pass == FormatPass::GBuffer) {
        vertStem = desc->gbufferShaderStem;
        fragStem = entry.writesMotionModel ? desc->gbufferFragmentStem
                                           : ENGINE_GBUFFER_FRAGMENT_STEM;
    } else if (pass == FormatPass::Shadow) {
        vertStem = desc->shadowShaderStem;
        fragStem = entry.shadowSamples ? desc->shadowFragmentStem
                                       : ENGINE_SHADOW_FRAGMENT_STEM;
    }

    // No GL stage: the pass keeps the program it used before stems
    const std::string vertPath = GLShaderPath(vertStem, GL_VERTEX_SUFFIX);
    std::error_code ec;
    if (vertStem.empty() || !std::filesystem::exists(vertPath, ec)) {
        if (!vertStem.empty())
            SLEAK_INFO(
                "OpenGL: vertex format {} has no {}, its {} pass keeps "
                "the engine program",
                format, vertPath, FORMAT_PASS_NAMES[slot]);
        return nullptr;
    }

    auto* program = new OpenGLShader();
    if (!program->compile(vertPath,
                          GLShaderPath(fragStem, GL_FRAGMENT_SUFFIX))) {
        SLEAK_ERROR(
            "OpenGL: vertex format {} {} program failed, its draws "
            "are skipped",
            format, FORMAT_PASS_NAMES[slot]);
        delete program;
        entry.failed[slot] = true;
        skip = true;
        return nullptr;
    }
    entry.programs[slot] = program;
    return program;
}

/// Binds the format's program or puts the pass program back.
void OpenGLRenderer::SelectFormatProgram(VertexFormatHandle format) {
    const FormatPass pass = CurrentFormatPass();
    const GLuint bound = OpenGLShader::BoundProgram();
    if (bound != m_formatProgram) {
        // A material or pass program was bound since the last switch
        m_formatProgram = 0;
        m_passProgram = bound;
    }
    m_formatDrawsSuppressed = false;
    if (pass == FormatPass::Count) return;

    bool skip = false;
    OpenGLShader* program = format != DEFAULT_VERTEX_LAYOUT
                                ? GetFormatProgram(format, pass, skip)
                                : nullptr;
    if (skip) {
        m_formatDrawsSuppressed = true;
        return;
    }
    // Formats without a GL shadow stage cast through the position program
    const bool legacyCaster = !program && pass == FormatPass::Shadow &&
                              format != DEFAULT_VERTEX_LAYOUT;
    if (legacyCaster) program = m_shadowPositionShader;

    if (program && program->GetProgram() != 0) {
        if (program->GetProgram() != bound) program->bind();
        m_formatProgram = program->GetProgram();
        if (legacyCaster) {
            RestorePassCull();
            m_shadowProgramSamples = false;
            return;
        }
        const FormatPrograms& entry = m_formatPrograms[format - 1];
        if (pass == FormatPass::GBuffer) {
            ApplyFormatCull(entry.cullMode);
            SetMotionModelWrites(entry.writesMotionModel);
        } else if (pass == FormatPass::Shadow) {
            ApplyFormatCull(entry.shadowCullMode);
            m_shadowProgramSamples = entry.shadowSamples;
            FlushShadowMaterial();
        }
        return;
    }

    // Default layout or a format without GL stages: the pass program draws
    if (m_formatProgram != 0) {
        OpenGLShader::BindProgram(m_passProgram);
        m_formatProgram = 0;
    }
    RestorePassCull();
    if (pass == FormatPass::GBuffer) SetMotionModelWrites(false);
    m_shadowProgramSamples = false;
}

/// Applies a format's cull mode, saving the pass state first.
void OpenGLRenderer::ApplyFormatCull(VertexCullMode mode) {
    if (mode == VertexCullMode::Default) {
        RestorePassCull();
        return;
    }
    if (!m_formatCullApplied) {
        m_savedCullEnabled = glIsEnabled(GL_CULL_FACE);
        glGetIntegerv(GL_CULL_FACE_MODE, &m_savedCullFace);
        m_formatCullApplied = true;
    }
    if (mode == VertexCullMode::None) {
        glDisable(GL_CULL_FACE);
        return;
    }
    glEnable(GL_CULL_FACE);
    glCullFace(mode == VertexCullMode::Front ? GL_FRONT : GL_BACK);
}

/// Restores the cull state a format changed.
void OpenGLRenderer::RestorePassCull() {
    if (!m_formatCullApplied) return;
    if (m_savedCullEnabled)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    glCullFace(static_cast<GLenum>(m_savedCullFace));
    m_formatCullApplied = false;
}

/// Forgets the switches of the previous pass.
void OpenGLRenderer::ResetFormatPassState() {
    m_formatProgram = 0;
    m_passProgram = 0;
    m_formatDrawsSuppressed = false;
    m_formatCullApplied = false;
    m_shadowMaterial = nullptr;
    m_shadowMaterialDirty = true;
    m_shadowProgramSamples = false;
}

/// Deletes every custom-format program.
void OpenGLRenderer::DestroyFormatPrograms() {
    for (FormatPrograms& entry : m_formatPrograms) {
        for (OpenGLShader*& program : entry.programs) {
            delete program;
            program = nullptr;
        }
    }
    m_formatPrograms.clear();
    m_formatProgram = 0;
}

/// Enables or masks writes to GBuffer RT3.
void OpenGLRenderer::SetMotionModelWrites(bool enabled) {
    if (enabled == m_motionModelWrites) return;
    const GLboolean mask = enabled ? GL_TRUE : GL_FALSE;
    glColorMaski(GBUFFER_MOTION_MODEL_INDEX, mask, mask, mask, mask);
    m_motionModelWrites = enabled;
}

// ============================================================
// SSAO pass — runs between GBuffer write and deferred lighting
// Output: R8 half-or-full-res AO in m_ssaoBlurTex, sampled by
// lighting_pass_gl.frag at texture unit 13 and multiplied into ao.
// ============================================================

bool OpenGLRenderer::CreateSSAOResources() {
    if (m_ssaoCreated) return true;
    if (!m_gbufferCreated) return false;

    int w = m_gbufferWidth;
    int h = m_gbufferHeight;

    auto makeR8Tex = [](GLuint& tex, int w, int h) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };

    // ---- Raw SSAO FBO ----
    makeR8Tex(m_ssaoTexture, w, h);
    glGenFramebuffers(1, &m_ssaoFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_ssaoTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SLEAK_ERROR("OpenGL SSAO FBO incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        CleanupSSAOResources();
        return false;
    }

    // ---- Blur FBO ----
    makeR8Tex(m_ssaoBlurTex, w, h);
    glGenFramebuffers(1, &m_ssaoBlurFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_ssaoBlurTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SLEAK_ERROR("OpenGL SSAO blur FBO incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        CleanupSSAOResources();
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // ---- 4x4 noise texture ----
    SSAOKernel kernelGen;
    kernelGen.Generate();

    glGenTextures(1, &m_ssaoNoiseTex);
    glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                 SSAOKernel::NOISE_SIZE, SSAOKernel::NOISE_SIZE, 0,
                 GL_RGBA, GL_FLOAT, kernelGen.noiseData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // ---- Shaders ----
    m_ssaoShader = new OpenGLShader();
    if (!m_ssaoShader->compile("assets/shaders/ssao_gl.vert",
                               "assets/shaders/ssao_gl.frag")) {
        SLEAK_ERROR("Failed to compile SSAO shader");
        CleanupSSAOResources();
        return false;
    }
    m_ssaoBlurShader = new OpenGLShader();
    if (!m_ssaoBlurShader->compile("assets/shaders/ssao_blur_gl.vert",
                                   "assets/shaders/ssao_blur_gl.frag")) {
        SLEAK_ERROR("Failed to compile SSAO blur shader");
        CleanupSSAOResources();
        return false;
    }

    // ---- UBOs ----
    // Settings (binding 0) — 64 bytes
    glGenBuffers(1, &m_ssaoSettingsUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_ssaoSettingsUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(SSAOSettingsGPUData), nullptr, GL_DYNAMIC_DRAW);

    // Kernel (binding 1) — 1024 bytes, upload once (deterministic seed)
    glGenBuffers(1, &m_ssaoKernelUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_ssaoKernelUBO);
    glBufferData(GL_UNIFORM_BUFFER,
                 sizeof(SSAOKernel::Sample) * SSAOKernel::MAX_KERNEL_SIZE,
                 kernelGen.samples.data(), GL_STATIC_DRAW);

    // Camera (binding 2) — Projection + View + InvViewProj = 3 * 64 = 192 bytes
    glGenBuffers(1, &m_ssaoCameraUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_ssaoCameraUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(float) * 16 * 3, nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    m_ssaoCreated = true;
    SLEAK_INFO("OpenGL SSAO ready ({}x{})", w, h);
    return true;
}

void OpenGLRenderer::CleanupSSAOResources() {
    if (m_ssaoFBO)         { glDeleteFramebuffers(1, &m_ssaoFBO);     m_ssaoFBO = 0; }
    if (m_ssaoBlurFBO)     { glDeleteFramebuffers(1, &m_ssaoBlurFBO); m_ssaoBlurFBO = 0; }
    if (m_ssaoTexture)     { glDeleteTextures(1, &m_ssaoTexture);     m_ssaoTexture = 0; }
    if (m_ssaoBlurTex)     { glDeleteTextures(1, &m_ssaoBlurTex);     m_ssaoBlurTex = 0; }
    if (m_ssaoNoiseTex)    { glDeleteTextures(1, &m_ssaoNoiseTex);    m_ssaoNoiseTex = 0; }
    if (m_ssaoSettingsUBO) { glDeleteBuffers(1, &m_ssaoSettingsUBO);  m_ssaoSettingsUBO = 0; }
    if (m_ssaoKernelUBO)   { glDeleteBuffers(1, &m_ssaoKernelUBO);    m_ssaoKernelUBO = 0; }
    if (m_ssaoCameraUBO)   { glDeleteBuffers(1, &m_ssaoCameraUBO);    m_ssaoCameraUBO = 0; }
    delete m_ssaoShader;     m_ssaoShader = nullptr;
    delete m_ssaoBlurShader; m_ssaoBlurShader = nullptr;
    m_ssaoCreated = false;
}

void OpenGLRenderer::RecreateSSAOOnResize(int width, int height) {
    if (!m_ssaoCreated) return;

    glBindTexture(GL_TEXTURE_2D, m_ssaoTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, m_ssaoBlurTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderer::ExecuteSSAOPass() {
    if (!m_ssaoCreated || !m_ssaoShader || !m_ssaoBlurShader) return;

    // Fast-path: if SSAO is disabled, clear the blur target to white so the
    // lighting shader's ao * ssao multiply is a no-op.
    if (!m_ssaoEnabled) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // ---- Upload per-frame settings UBO ----
    SSAOSettingsGPUData s{};
    s.Radius       = m_ssaoRadius;
    s.Bias         = m_ssaoBias;
    s.Power        = m_ssaoPower;
    s.KernelSize   = SSAOKernel::MAX_KERNEL_SIZE;
    s.ScreenWidth  = static_cast<float>(m_gbufferWidth);
    s.ScreenHeight = static_cast<float>(m_gbufferHeight);
    s.SSAOEnabled  = 1;
    s.NearPlane    = 0.1f;
    s.FarPlane     = 2000.0f;

    glBindBuffer(GL_UNIFORM_BUFFER, m_ssaoSettingsUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(s), &s);

    // ---- Upload Camera UBO (Projection, View, InvViewProj) ----
    const Math::Matrix4& V = Camera::GetMainViewMatrix();
    const Math::Matrix4& P = Camera::GetMainProjectionMatrix();
    float camData[16 * 3];
    std::memcpy(camData,       &P(0,0), sizeof(float) * 16);
    std::memcpy(camData + 16,  &V(0,0), sizeof(float) * 16);
    // InvViewProj = inverse(View * Proj) — reconstructs world pos from depth.
    // Row-vector engine convention: clip = world * (View * Proj).
    Math::Matrix4 VP = V * P;
    float invP[16];
    {
        // Inline determinant-based 4x4 inverse (Cramer). Returns identity if singular.
        const float* m = &VP(0,0);
        float i0  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
        float i4  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
        float i8  =  m[4]*m[9] *m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
        float i12 = -m[4]*m[9] *m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
        float det = m[0]*i0 + m[1]*i4 + m[2]*i8 + m[3]*i12;
        if (std::fabs(det) < 1e-8f) {
            std::memset(invP, 0, sizeof(invP));
            invP[0] = invP[5] = invP[10] = invP[15] = 1.0f;
        } else {
            float id = 1.0f / det;
            invP[0]  = i0 * id; invP[4]  = i4 * id; invP[8]  = i8 * id; invP[12] = i12 * id;
            invP[1]  = (-m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10]) * id;
            invP[5]  = ( m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10]) * id;
            invP[9]  = (-m[0]*m[9] *m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9] ) * id;
            invP[13] = ( m[0]*m[9] *m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9] ) * id;
            invP[2]  = ( m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6]) * id;
            invP[6]  = (-m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6]) * id;
            invP[10] = ( m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5]) * id;
            invP[14] = (-m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5]) * id;
            invP[3]  = (-m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6]) * id;
            invP[7]  = ( m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6]) * id;
            invP[11] = (-m[0]*m[5]*m[11] + m[0]*m[7]*m[9]  + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7] + m[8]*m[3]*m[5]) * id;
            invP[15] = ( m[0]*m[5]*m[10] - m[0]*m[6]*m[9]  - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6] - m[8]*m[2]*m[5]) * id;
        }
    }
    std::memcpy(camData + 32, invP, sizeof(float) * 16);

    glBindBuffer(GL_UNIFORM_BUFFER, m_ssaoCameraUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(camData), camData);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // Bind UBOs at private slots 7/8/9 — slots 0-6 are used by the lighting
    // pass (LightUBO=2, ShadowUBO=5, DeferredCB=6, material/transform=0,1,3).
    // Binding SSAO's UBOs to those would silently nuke the lighting pass.
    glBindBufferBase(GL_UNIFORM_BUFFER, 7, m_ssaoSettingsUBO);
    glBindBufferBase(GL_UNIFORM_BUFFER, 8, m_ssaoKernelUBO);
    glBindBufferBase(GL_UNIFORM_BUFFER, 9, m_ssaoCameraUBO);

    // ---- Pass 1: SSAO ----
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
    ApplyViewport(0, 0, m_gbufferWidth, m_gbufferHeight);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_ssaoShader->bind();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_gbufferDepth);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_gbufferNormalRough);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTex);
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(m_lightingVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ---- Pass 2: Blur ----
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
    glClear(GL_COLOR_BUFFER_BIT);

    m_ssaoBlurShader->bind();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_ssaoTexture);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Restore depth/blend state for the upcoming lighting pass
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

}  // namespace RenderEngine
}  // namespace Sleak
