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
    static constexpr int SHADOW_MAP_SIZE = 4096;
    GLuint m_shadowFBO = 0;
    GLuint m_shadowDepthTex = 0;
    GLuint m_shadowUBO = 0;
    bool m_shadowMapCreated = false;
    bool m_shadowUBOCreated = false;
    float m_lightVP[16] = {};
    float m_pendingLightVP[16] = {};
    bool  m_hasPendingLightVP = false;
    bool m_inShadowPass = false;
    GLuint m_shadowTransformUBO = 0;
    void SetLightVP(const float* mat) override;
    void UpdateShadowLightUBO(const void* data, uint32_t size) override;
    bool CreateShadowMapResources();
    bool CreateShadowUBO();
    void RenderShadowPass();

    // ---- Deferred rendering (GBuffer) ----
    // Deferred mode overrides
    virtual bool IsDeferredEnabled() const override;
    virtual bool IsInGeometryPass() const override { return m_inGeometryPass; }
    virtual void BindGBufferShader() override;
    virtual void ExecuteDeferredLightingPass() override;
    virtual void BeginForwardTransparentPass() override;
    virtual void EndForwardTransparentPass() override;
    virtual void UpdateDeferredCB(const void* data, uint32_t size) override;

    // GBuffer FBO + textures
    GLuint m_gbufferFBO          = 0;
    GLuint m_gbufferAlbedoAO     = 0;   // RT0: RGBA8  — Albedo + AO
    GLuint m_gbufferNormalRough  = 0;   // RT1: RGBA16F — Normal + Roughness
    GLuint m_gbufferMetalEmit    = 0;   // RT2: RGBA8  — Metallic + EmissiveScale
    GLuint m_gbufferWorldPos     = 0;   // RT3: RGBA32F — WorldPos.xyz (a unused)
    GLuint m_gbufferDepth        = 0;   // Depth texture (for lighting pass sampling)
    bool   m_gbufferCreated      = false;
    int    m_gbufferWidth        = 0;
    int    m_gbufferHeight       = 0;

    // GBuffer + lighting pass shaders
    class OpenGLShader* m_gbufferShader  = nullptr;   // geometry pass shader
    class OpenGLShader* m_lightingShader = nullptr;   // lighting pass shader
    GLuint m_lightingVAO = 0;  // empty VAO for fullscreen triangle

    // Deferred CB UBO (InvViewProj + screen size; bound at slot 6)
    GLuint m_deferredCBUBO    = 0;
    bool   m_deferredCBCreated = false;

    bool m_inGeometryPass          = false;
    bool m_inForwardTransparentPass = false;

    bool CreateGBufferResources();
    void CleanupGBufferResources();
    void RecreateGBufferOnResize(int width, int height);

    // ---- SSAO ----
    GLuint m_ssaoFBO        = 0;
    GLuint m_ssaoTexture    = 0;   // R8: raw AO
    GLuint m_ssaoBlurFBO    = 0;
    GLuint m_ssaoBlurTex    = 0;   // R8: blurred AO (consumed by lighting)
    GLuint m_ssaoNoiseTex   = 0;   // 4x4 RGBA16F
    GLuint m_ssaoSettingsUBO = 0;  // binding 0
    GLuint m_ssaoKernelUBO   = 0;  // binding 1
    GLuint m_ssaoCameraUBO   = 0;  // binding 2 (Projection/View/InvProjection)
    class OpenGLShader* m_ssaoShader     = nullptr;
    class OpenGLShader* m_ssaoBlurShader = nullptr;
    bool   m_ssaoCreated = false;

    bool CreateSSAOResources();
    void CleanupSSAOResources();
    void RecreateSSAOOnResize(int width, int height);
    void ExecuteSSAOPass();

    // ---- IBL (Image-Based Lighting) ----
    class OpenGLIBL* m_ibl    = nullptr;
    GLuint m_iblSettingsUBO   = 0;   // binding 10 in lighting_pass_gl.frag
    GLuint m_iblBoundCubemap  = 0;   // last skybox cubemap we baked from
    void EnsureIBL();
    void CleanupIBL();
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _OPENGLRENDERER_H
