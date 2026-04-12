#include "../../include/private/Graphics/OpenGL/OpenGLRenderer.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLBuffer.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLShader.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLTexture.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLCubemapTexture.hpp"
#include "Graphics/Vertex.hpp"
#include "Graphics/ResourceManager.hpp"
#include "Graphics/ConstantBuffer.hpp"
#include "Graphics/RenderCommandQueue.hpp"
#include <SDL3/SDL.h>
#include <vector>
#include <cstring>

namespace Sleak {
namespace RenderEngine {

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
        [](const void* data, uint32_t w, uint32_t h, TextureFormat fmt) -> Texture* {
            auto* tex = new OpenGLTexture();
            if (tex->LoadFromMemory(data, w, h, fmt)) return tex;
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
    glViewport(0, 0, width, height);

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
    glGenVertexArrays(1, &m_VAO);
}

void OpenGLRenderer::BeginRender() {
    if (m_vsyncChangeRequested)
        ApplyVSyncChange();
    if (m_msaaChangeRequested)
        ApplyMSAAChange();

    // Shadow pass before main rendering
    if (m_shadowPassEnabled) {
        RenderShadowPass();
    }

    if (m_gbufferCreated && m_deferredEnabled) {
        // Deferred path: geometry pass writes to GBuffer
        glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);
        GLenum drawBuffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
        glDrawBuffers(3, drawBuffers);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // Geometry pass must NOT blend — each pixel writes final GBuffer values
        glDisable(GL_BLEND);
        m_inGeometryPass = true;
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

    if (bImInitialized) {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    glBindVertexArray(0);
    SDL_GL_SwapWindow(m_Window->GetSDLWindow());

    UpdateFrameMetrics();
}

void OpenGLRenderer::Cleanup() {
    if (m_Initialized) {
        CleanupGBufferResources();
        if (m_shadowTransformUBO) { glDeleteBuffers(1, &m_shadowTransformUBO); m_shadowTransformUBO = 0; }
        if (m_shadowUBO) { glDeleteBuffers(1, &m_shadowUBO); m_shadowUBO = 0; }
        if (m_shadowDepthTex) { glDeleteTextures(1, &m_shadowDepthTex); m_shadowDepthTex = 0; }
        if (m_shadowFBO) { glDeleteFramebuffers(1, &m_shadowFBO); m_shadowFBO = 0; }
        m_shadowMapCreated = false;
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

void OpenGLRenderer::Resize(uint32_t width, uint32_t height) {
    glViewport(0, 0, width, height);
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

void OpenGLRenderer::Draw(uint32_t vertexCount) {
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawArrays(mode, 0, vertexCount);
    DrawnVertices += vertexCount;
    DrawnTriangles += vertexCount / 3;
}

void OpenGLRenderer::DrawIndexed(uint32_t indexCount) {
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawElements(mode, indexCount, GL_UNSIGNED_INT, 0);
    DrawnVertices += indexCount;
    DrawnTriangles += indexCount / 3;
}

void OpenGLRenderer::DrawInstance(uint32_t instanceCount,
                                   uint32_t vertexPerInstance) {
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawArraysInstanced(mode, 0, vertexPerInstance,
                          instanceCount);
    DrawnVertices += vertexPerInstance * instanceCount;
    DrawnTriangles += (vertexPerInstance / 3) * instanceCount;
}

void OpenGLRenderer::DrawIndexedInstance(uint32_t instanceCount,
                                          uint32_t indexPerInstance) {
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawElementsInstanced(mode, indexPerInstance,
                            GL_UNSIGNED_INT, 0, instanceCount);
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
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
               static_cast<GLsizei>(width),
               static_cast<GLsizei>(height));
    glDepthRangef(minDepth, maxDepth);
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

    glBindBuffer(GL_ARRAY_BUFFER, glBuf->GetGLBuffer());

    // Position: 3 floats at offset 0
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, px));

    // Normal: 3 floats at offset 12
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, nx));

    // Tangent: 4 floats at offset 24
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, tx));

    // Color: 4 floats at offset 40
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, r));

    // UV: 2 floats at offset 56
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, u));

    // BoneIDs: 4 ints (must use IPointer for integer attributes)
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 4, GL_INT, sizeof(Vertex),
                           (void*)offsetof(Vertex, boneIDs));

    // BoneWeights: 4 floats
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, boneWeights));
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
    if (m_inShadowPass && slot == 0 && glBuf->GetCPUShadowCopySize() >= 128) {
        const float* srcWorld = reinterpret_cast<const float*>(
            static_cast<const char*>(glBuf->GetCPUShadowCopy()) + 64);

        float shadowPC[32];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += srcWorld[r * 4 + k] * m_lightVP[k * 4 + c];
                }
                shadowPC[r * 4 + c] = sum;
            }
        }
        memcpy(&shadowPC[16], srcWorld, sizeof(float) * 16);

        // Create dedicated shadow transform UBO on first use
        if (m_shadowTransformUBO == 0) {
            glGenBuffers(1, &m_shadowTransformUBO);
            glBindBuffer(GL_UNIFORM_BUFFER, m_shadowTransformUBO);
            glBufferData(GL_UNIFORM_BUFFER, 128, nullptr, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }

        glBindBuffer(GL_UNIFORM_BUFFER, m_shadowTransformUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(shadowPC), shadowPC);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
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
    if (mat) memcpy(m_lightVP, mat, sizeof(m_lightVP));
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

    RenderEngine::PCSSShadowGPUData shadowData{};
    memcpy(shadowData.LightVP, ubo->LightVP, sizeof(float) * 16);
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

bool OpenGLRenderer::CreateShadowMapResources() {
    if (m_shadowMapCreated) return true;

    // Create depth texture
    glGenTextures(1, &m_shadowDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
                 SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0,
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
    SLEAK_INFO("OpenGL shadow map resources created ({}x{})", SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    return true;
}

void OpenGLRenderer::RenderShadowPass() {
    // Skip if no cached draws — preserve previous frame's shadow map
    auto* queue = RenderCommandQueue::GetInstance();
    if (!queue || !queue->HasCachedShadowDraws()) return;

    if (!m_shadowMapCreated) {
        if (!CreateShadowMapResources()) return;
    }

    // Save current viewport
    GLint savedViewport[4];
    glGetIntegerv(GL_VIEWPORT, savedViewport);

    // Bind shadow FBO
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
    glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    glClear(GL_DEPTH_BUFFER_BIT);

    // Enable polygon offset for slope-scale depth bias
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);

    // Disable face culling during shadow pass — all faces must cast shadow
    // (front-face culling removed the outward-facing surfaces of blocks)
    glDisable(GL_CULL_FACE);

    // Ensure VAO is bound for shadow pass draws
    glBindVertexArray(m_VAO);

    // Execute shadow draw commands
    m_inShadowPass = true;
    if (queue) {
        queue->ExecuteShadowPass(this);
    }
    m_inShadowPass = false;

    // Restore state
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE); // re-enable culling for main pass
    ConfigureRenderFace();

    // Restore framebuffer
    if (m_msaaFBO != 0)
        glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);
    else
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);

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
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0,
                     (internalFmt == GL_RGBA16F) ? GL_RGBA : GL_RGBA,
                     (internalFmt == GL_RGBA16F) ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };

    // RT0: Albedo + AO   (RGBA8)
    makeColorTex(m_gbufferAlbedoAO,    GL_RGBA8,   w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gbufferAlbedoAO, 0);

    // RT1: Normal + Roughness  (RGBA16F)
    makeColorTex(m_gbufferNormalRough, GL_RGBA16F, w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gbufferNormalRough, 0);

    // RT2: Metallic + Emissive  (RGBA8)
    makeColorTex(m_gbufferMetalEmit,   GL_RGBA8,   w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_gbufferMetalEmit, 0);

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

    GLenum drawBuffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
    glDrawBuffers(3, drawBuffers);

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
    return true;
}

void OpenGLRenderer::CleanupGBufferResources() {
    if (m_gbufferAlbedoAO)    { glDeleteTextures(1, &m_gbufferAlbedoAO);    m_gbufferAlbedoAO    = 0; }
    if (m_gbufferNormalRough) { glDeleteTextures(1, &m_gbufferNormalRough); m_gbufferNormalRough = 0; }
    if (m_gbufferMetalEmit)   { glDeleteTextures(1, &m_gbufferMetalEmit);   m_gbufferMetalEmit   = 0; }
    if (m_gbufferDepth)       { glDeleteTextures(1, &m_gbufferDepth);       m_gbufferDepth       = 0; }
    if (m_gbufferFBO)         { glDeleteFramebuffers(1, &m_gbufferFBO);     m_gbufferFBO         = 0; }
    if (m_lightingVAO)        { glDeleteVertexArrays(1, &m_lightingVAO);    m_lightingVAO        = 0; }
    if (m_deferredCBUBO)      { glDeleteBuffers(1, &m_deferredCBUBO);       m_deferredCBUBO      = 0; }
    delete m_gbufferShader;  m_gbufferShader  = nullptr;
    delete m_lightingShader; m_lightingShader = nullptr;
    m_gbufferCreated    = false;
    m_deferredCBCreated = false;
}

void OpenGLRenderer::RecreateGBufferOnResize(int width, int height) {
    if (!m_gbufferCreated) return;

    m_gbufferWidth  = width;
    m_gbufferHeight = height;

    // Resize AlbedoAO
    glBindTexture(GL_TEXTURE_2D, m_gbufferAlbedoAO);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Resize NormalRough
    glBindTexture(GL_TEXTURE_2D, m_gbufferNormalRough);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);

    // Resize MetalEmit
    glBindTexture(GL_TEXTURE_2D, m_gbufferMetalEmit);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Resize depth
    glBindTexture(GL_TEXTURE_2D, m_gbufferDepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);
    SLEAK_INFO("OpenGL GBuffer resized to {}x{}", width, height);
}

void OpenGLRenderer::BindGBufferShader() {
    if (m_gbufferShader) {
        // Ensure GBuffer FBO is active with all 3 color attachments
        glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);
        GLenum drawBuffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
        glDrawBuffers(3, drawBuffers);
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

void OpenGLRenderer::UpdateDeferredCB(const void* data, uint32_t size) {
    if (!m_deferredCBCreated) return;
    glBindBuffer(GL_UNIFORM_BUFFER, m_deferredCBUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, size, data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 6, m_deferredCBUBO);
}

void OpenGLRenderer::ExecuteDeferredLightingPass() {
    m_inGeometryPass = false;

    if (!m_gbufferCreated || !m_lightingShader) return;

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
    m_lightingShader->bind();

    // Bind GBuffer textures at units 8-11
    glActiveTexture(GL_TEXTURE8);  glBindTexture(GL_TEXTURE_2D, m_gbufferAlbedoAO);
    glActiveTexture(GL_TEXTURE9);  glBindTexture(GL_TEXTURE_2D, m_gbufferNormalRough);
    glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_2D, m_gbufferMetalEmit);
    glActiveTexture(GL_TEXTURE11); glBindTexture(GL_TEXTURE_2D, m_gbufferDepth);
    glActiveTexture(GL_TEXTURE0);

    // Rebind shadow map at unit 3 — BindGBufferShader cleared it to avoid
    // sampler2DShadow/sampler2D conflict during the geometry pass.
    if (m_shadowMapCreated) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);
        glActiveTexture(GL_TEXTURE0);
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
    // Transparent objects need alpha blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void OpenGLRenderer::EndForwardTransparentPass() {
    m_inForwardTransparentPass = false;
}

}  // namespace RenderEngine
}  // namespace Sleak
