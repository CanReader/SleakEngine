#ifndef _OPENGLRENDERER_H
#define _OPENGLRENDERER_H

#include <Core/OSDef.hpp>
#include "../Renderer.hpp"
#include "../RenderContext.hpp"
#include "../../Window.hpp"
#include <SDL3/SDL.h>
#include <glad/glad.h>
#include <array>
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

namespace Sleak {

namespace RenderEngine {

class ENGINE_API OpenGLRenderer : public Renderer, public RenderContext {
public:
    OpenGLRenderer(Window* window);
    virtual ~OpenGLRenderer();

    bool Initialize() override;
    void BeginRender() override;
    void EndRender() override;
    void Cleanup() override;

    virtual void Resize(uint32_t width, uint32_t height) override;

    virtual bool CreateImGUI() override;

    void ApplyMSAAChange() override;
    void ApplyVSyncChange() override;

    virtual RenderContext* GetContext() override { return this; }

    // RenderContext interface
    virtual void Draw(uint32_t vertexCount) override;
    virtual void DrawIndexed(uint32_t indexCount) override;
    virtual void DrawInstance(uint32_t instanceCount,
                              uint32_t vertexPerInstance) override;
    virtual void DrawIndexedInstance(uint32_t instanceCount,
                                     uint32_t indexPerInstance) override;

    virtual void SetRenderFace(RenderFace face) override;
    virtual void SetRenderMode(RenderMode mode) override;
    virtual void SetViewport(float x, float y, float width, float height,
                             float minDepth = 0.0f,
                             float maxDepth = 1.0f) override;
    virtual void ClearRenderTarget(float r, float g, float b,
                                   float a) override;
    virtual void ClearDepthStencil(bool clearDepth, bool clearStencil,
                                   float depth, uint8_t stencil) override;

    virtual void SetDepthWrite(bool enabled) override;
    virtual void SetDepthCompare(DepthCompare compare) override;
    virtual void SetCullEnabled(bool enabled) override;
    virtual void BindTexture(RefPtr<Sleak::Texture> texture, uint32_t slot = 0) override;

    virtual void BindVertexBuffer(RefPtr<BufferBase> buffer,
                                  uint32_t slot = 0) override;
    virtual void BindIndexBuffer(RefPtr<BufferBase> buffer,
                                 uint32_t slot = 0) override;
    virtual void BindConstantBuffer(RefPtr<BufferBase> buffer,
                                    uint32_t slot = 0) override;
    virtual void BindBoneBuffer(RefPtr<BufferBase> buffer) override;

    virtual void BeginDebugLinePass() override;
    virtual void EndDebugLinePass() override;

    virtual BufferBase* CreateBuffer(BufferType Type, uint32_t size,
                                     void* data) override;
    virtual Shader* CreateShader(const std::string& shaderSource) override;
    virtual Texture* CreateTexture(const std::string& TexturePath) override;
    virtual Texture* CreateTextureFromData(uint32_t width, uint32_t height,
                                           void* data) override;

    Texture* CreateCubemapTexture(const std::array<std::string, 6>& facePaths);
    Texture* CreateCubemapTextureFromPanorama(const std::string& panoramaPath);

private:
    Window* m_Window;
    SDL_GLContext m_GLContext = nullptr;
    bool m_Initialized = false;

    GLuint m_VAO = 0;
    bool m_debugLineMode = false;

    // MSAA FBO resources
    GLuint m_msaaFBO = 0;
    GLuint m_msaaColorRBO = 0;
    GLuint m_msaaDepthRBO = 0;
    void CreateMSAAFramebuffer();
    void CleanupMSAAFramebuffer();

    virtual void ConfigureRenderMode() override;
    virtual void ConfigureRenderFace() override;

    void SetupVertexLayout();

    // Shadow mapping
    static constexpr int SHADOW_MAP_SIZE = 2048;
    GLuint m_shadowFBO = 0;
    GLuint m_shadowDepthTex = 0;
    GLuint m_shadowUBO = 0;
    bool m_shadowMapCreated = false;
    bool m_shadowUBOCreated = false;
    float m_lightVP[16] = {};
    bool m_inShadowPass = false;
    GLuint m_shadowTransformUBO = 0;
    void SetLightVP(const float* mat) override;
    void UpdateShadowLightUBO(const void* data, uint32_t size) override;
    bool CreateShadowMapResources();
    bool CreateShadowUBO();
    void RenderShadowPass();
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _OPENGLRENDERER_H
