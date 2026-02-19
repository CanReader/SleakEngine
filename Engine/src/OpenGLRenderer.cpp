#include "../../include/private/Graphics/OpenGL/OpenGLRenderer.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLBuffer.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLShader.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLTexture.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLCubemapTexture.hpp"
#include "Graphics/Vertex.hpp"
#include "Graphics/ResourceManager.hpp"
#include <vector>

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

    SetupVertexLayout();

    m_Initialized = true;

    SLEAK_INFO("OpenGL renderer has been initialized successfully!");
    SLEAK_INFO("OpenGL Version: {}",
               (const char*)glGetString(GL_VERSION));

    return true;
}

void OpenGLRenderer::SetupVertexLayout() {
    glGenVertexArrays(1, &m_VAO);
}

void OpenGLRenderer::BeginRender() {
    glClearColor(0.39f, 0.58f, 0.93f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glBindVertexArray(m_VAO);

    if (bImInitialized) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }
}

void OpenGLRenderer::EndRender() {
    if (bImInitialized) {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    glBindVertexArray(0);
    SDL_GL_SwapWindow(m_Window->GetSDLWindow());
}

void OpenGLRenderer::Cleanup() {
    if (m_Initialized) {
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
}

// -----------------------------------------------------------------------
// RenderContext Implementation
// -----------------------------------------------------------------------

void OpenGLRenderer::Draw(uint32_t vertexCount) {
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawArrays(mode, 0, vertexCount);
}

void OpenGLRenderer::DrawIndexed(uint32_t indexCount) {
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawElements(mode, indexCount, GL_UNSIGNED_INT, 0);
}

void OpenGLRenderer::DrawInstance(uint32_t instanceCount,
                                   uint32_t vertexPerInstance) {
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawArraysInstanced(mode, 0, vertexPerInstance,
                          instanceCount);
}

void OpenGLRenderer::DrawIndexedInstance(uint32_t instanceCount,
                                          uint32_t indexPerInstance) {
    GLenum mode = m_debugLineMode ? GL_LINES : GL_TRIANGLES;
    glDrawElementsInstanced(mode, indexPerInstance,
                            GL_UNSIGNED_INT, 0, instanceCount);
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

    glBindBufferBase(GL_UNIFORM_BUFFER, slot, glBuf->GetGLBuffer());
}

void OpenGLRenderer::BindBoneBuffer(RefPtr<BufferBase> buffer) {
    BindConstantBuffer(buffer, 3);
}

BufferBase* OpenGLRenderer::CreateBuffer(BufferType type, uint32_t size,
                                          void* data) {
    auto* buffer = new OpenGLBuffer(size, type);
    buffer->Initialize(data);
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

}  // namespace RenderEngine
}  // namespace Sleak
