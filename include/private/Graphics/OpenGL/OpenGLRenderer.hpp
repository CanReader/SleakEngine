#ifndef _OPENGLRENDERER_H
#define _OPENGLRENDERER_H

#include <SDL3/SDL.h>
#include <backends/imgui_impl_opengl3.h>
#include <glad/glad.h>
#include <imgui.h>

#include <Core/OSDef.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../../Core/Window.hpp"
#include "../Common/RenderContext.hpp"
#include "../Common/Renderer.hpp"

namespace Sleak {

struct VertexLayoutDesc;

namespace RenderEngine {

/// OpenGL 3.3+ backend: deferred GBuffer pipeline, SSAO, shadow mapping, and baked IBL.
class ENGINE_API OpenGLRenderer : public Renderer, public RenderContext {
public:
    OpenGLRenderer(Window* window);
    virtual ~OpenGLRenderer();

    bool Initialize() override;
    void BeginRender() override;
    void EndRender() override;
    void Cleanup() override;
#ifdef SLEAK_HOT_RELOAD
    /// Relinks every live shader built from one of the given stems.
    bool ReloadShaders(const std::vector<std::string>& stems) override;
#endif

    virtual uint32_t GetFeatureCaps() const override {
        return CapDeferred | CapSSAO | CapIBL | CapShadows;
    }

    /// Arms a UI-free readback of the next presented frame into a binary PPM.
    bool RequestFrameCapture(const std::string& path) override;
    /// True while an armed capture has not been written yet.
    bool IsFrameCapturePending() const override {
        return !m_capturePath.empty();
    }

    virtual void Resize(uint32_t width, uint32_t height) override;

    virtual bool CreateImGUI() override;

    void ApplyMSAAChange() override;
    void ApplyVSyncChange() override;

    virtual RenderContext* GetContext() override { return this; }

    // RenderContext interface
    virtual void Draw(uint32_t vertexCount) override;
    virtual void Draw(uint32_t vertexCount, uint32_t firstVertex) override;
    virtual void DrawIndexed(uint32_t indexCount) override;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t firstIndex,
                             int32_t baseVertex) override;
    virtual void DrawInstance(uint32_t instanceCount,
                              uint32_t vertexPerInstance) override;
    virtual void DrawIndexedInstance(uint32_t instanceCount,
                                     uint32_t indexPerInstance) override;
    virtual void DrawIndexedInstance(uint32_t instanceCount,
                                     uint32_t indexPerInstance,
                                     uint32_t firstIndex,
                                     int32_t baseVertex) override;

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

    /// Records the caster material; alpha-tested shadow programs sample its
    /// diffuse at texture unit 0.
    virtual void BindShadowMaterial(Sleak::Material* material) override;

    virtual BufferBase* CreateBuffer(BufferType Type, uint32_t size,
                                     void* data) override;
    virtual Shader* CreateShader(const std::string& shaderSource) override;
    virtual Texture* CreateTexture(const std::string& TexturePath) override;
    virtual Texture* CreateTextureFromData(uint32_t width, uint32_t height,
                                           void* data) override;

    /// Loads a cubemap from six face image paths.
    Texture* CreateCubemapTexture(const std::array<std::string, 6>& facePaths);
    /// Loads a cubemap by converting a single equirectangular panorama.
    Texture* CreateCubemapTextureFromPanorama(const std::string& panoramaPath);

private:
    Window* m_Window;
    SDL_GLContext m_GLContext = nullptr;
    bool m_Initialized = false;

    // -capture target, empty once written
    std::string m_capturePath;
    /// Reads the finished back buffer into the armed PPM and disarms it.
    void WriteFrameCapture();

    GLuint m_VAO = 0;
    bool m_debugLineMode = false;

    // Vertex layout currently applied to m_VAO
    static constexpr uint32_t NO_CONFIGURED_LAYOUT = UINT32_MAX;
    uint32_t m_configuredLayout = NO_CONFIGURED_LAYOUT;
    const VertexLayoutDesc* m_configuredLayoutDesc = nullptr;
    uint32_t m_enabledAttribMask = 0;

    // Last viewport set outside the shadow pass (x, y, width, height)
    std::array<GLint, 4> m_viewport = {};
    /// Sets the GL viewport and records it so passes can restore it later.
    void ApplyViewport(GLint x, GLint y, GLsizei width, GLsizei height);

    // MSAA FBO resources
    GLuint m_msaaFBO = 0;
    GLuint m_msaaColorRBO = 0;
    GLuint m_msaaDepthRBO = 0;
    /// Allocates the multisampled color/depth renderbuffers sized to the current swapchain.
    void CreateMSAAFramebuffer();
    void CleanupMSAAFramebuffer();

    virtual void ConfigureRenderMode() override;
    virtual void ConfigureRenderFace() override;

    /// Creates the shared mesh VAO and resets its cached attribute layout.
    void SetupVertexLayout();
    /// Applies a vertex layout's attribute formats to m_VAO unless it is
    /// already the active one; a null desc selects the default Vertex.
    void ConfigureVertexLayout(uint32_t layout, const VertexLayoutDesc* desc);

    // Shadow mapping
    GLuint m_shadowFBO = 0;
    GLuint m_shadowDepthTex = 0;
    GLuint m_shadowUBO = 0;
    bool m_shadowMapCreated = false;
    bool m_shadowUBOCreated = false;
    float m_lightVP[16] = {};
    float m_pendingLightVP[16] = {};
    bool  m_hasPendingLightVP = false;
    // Matrix the shadow map was last rendered with
    float m_shadowMapLightVP[16] = {};
    bool m_shadowMapRendered = false;
    bool m_inShadowPass = false;
    GLuint m_shadowTransformUBO = 0;
    // Last shadow transform uploaded: [LightVP*World][World], keyed on World
    float m_shadowTransformCache[32] = {};
    float m_shadowWorldCache[16] = {};
    bool m_shadowTransformCacheValid = false;
    void SetLightVP(const float* mat) override;
    /// Uploads light and shadow data, sampling with the matrix the shadow
    /// map was rendered with.
    void UpdateShadowLightUBO(const void* data, uint32_t size) override;
    /// Uploads this frame's FrameData and game block (buffers 11 and 12).
    void UploadFrameData() override;
    /// Allocates the shadow-pass depth FBO and texture at the configured resolution.
    bool CreateShadowMapResources();
    /// Allocates the UBO backing ShadowLightUBO.
    bool CreateShadowUBO();
    /// Replays cached shadow-caster draws into the depth-only shadow FBO.
    void RenderShadowPass();
    // Engine shadow programs: default layout, position-only for formats
    class OpenGLShader* m_shadowDepthShader = nullptr;
    class OpenGLShader* m_shadowPositionShader = nullptr;
    bool m_shadowProgramsCreated = false;
    /// Compiles the engine shadow depth programs once.
    void CreateShadowPrograms();

    // ---- Deferred rendering (GBuffer) ----
    // Deferred mode overrides
    virtual bool IsDeferredEnabled() const override;
    virtual bool IsInGeometryPass() const override { return m_inGeometryPass; }
    virtual void BindGBufferShader() override;
    virtual void BindPBRMaterial(Sleak::Material* material) override;
    virtual void ExecuteDeferredLightingPass() override;
    virtual void BeginForwardTransparentPass() override;
    virtual void EndForwardTransparentPass() override;
    virtual void UpdateDeferredCB(const void* data, uint32_t size) override;

    // GBuffer FBO + textures
    GLuint m_gbufferFBO          = 0;
    GLuint m_gbufferAlbedoAO     = 0;   // RT0: RGBA8  — Albedo + AO
    GLuint m_gbufferNormalRough  = 0;   // RT1: RGBA16F — Normal + Roughness
    GLuint m_gbufferMetalEmit    = 0;   // RT2: RGBA8  — Metallic + EmissiveScale
    GLuint m_gbufferMotionModel = 0;    // RT3: RGBA16F motion, game, model
    GLuint m_gbufferDepth        = 0;   // Depth texture (world pos reconstructed)
    bool   m_gbufferCreated      = false;
    int    m_gbufferWidth        = 0;
    int    m_gbufferHeight       = 0;

    // GBuffer + lighting pass shaders
    class OpenGLShader* m_gbufferShader  = nullptr;   // geometry pass shader
    class OpenGLShader* m_lightingShader = nullptr;   // lighting pass shader
    // Game lighting stem (SetLightingShader); null keeps m_lightingShader
    class OpenGLShader* m_lightingOverrideShader = nullptr;
    // Stem whose failure was already logged
    std::string m_lightingOverrideFailedStem;
    /// Rebuilds or drops the lighting override after SetLightingShader,
    /// keeping the engine shader and logging once when the stem fails.
    void ApplyLightingShaderChange();
    GLuint m_lightingVAO = 0;  // empty VAO for fullscreen triangle

    // Deferred CB UBO (InvViewProj + screen size; bound at slot 6)
    GLuint m_deferredCBUBO    = 0;
    bool   m_deferredCBCreated = false;

    bool m_inGeometryPass          = false;
    bool m_inForwardTransparentPass = false;

    // ---- Custom vertex format programs ----
    /// Pass a custom-format program is resolved for.
    enum class FormatPass : uint8_t { GBuffer, Shadow, Transparent, Count };
    static constexpr size_t FORMAT_PASS_COUNT =
        static_cast<size_t>(FormatPass::Count);
    /// One registered layout's programs per pass. A resolved pass without a
    /// program has no _gl.vert and keeps the pass program; failed skips draws.
    struct FormatPrograms {
        std::array<OpenGLShader*, FORMAT_PASS_COUNT> programs{};
        std::array<bool, FORMAT_PASS_COUNT> resolved{};
        std::array<bool, FORMAT_PASS_COUNT> failed{};
        bool writesMotionModel = false;  // has a gbufferFragmentStem
        bool shadowSamples = false;      // has a shadowFragmentStem
        VertexCullMode cullMode = VertexCullMode::Default;
        VertexCullMode shadowCullMode = VertexCullMode::Default;
    };
    // Indexed by handle - 1 (handles are dense and start at 1)
    std::vector<FormatPrograms> m_formatPrograms;
    // Program the last format switch bound, 0 while the pass program is
    GLuint m_formatProgram = 0;
    // Material or pass program that default-layout draws go back to
    GLuint m_passProgram = 0;
    bool m_formatDrawsSuppressed = false;
    // Cull state a format replaced in this pass, restored when it ends
    bool m_formatCullApplied = false;
    GLboolean m_savedCullEnabled = GL_FALSE;
    GLint m_savedCullFace = GL_BACK;
    // glColorMaski state of GBuffer RT3
    bool m_motionModelWrites = true;
    // Caster material for alpha-tested shadow programs
    Sleak::Material* m_shadowMaterial = nullptr;
    bool m_shadowMaterialDirty = true;
    bool m_shadowProgramSamples = false;
    /// Pass the next custom-format draw resolves a program for; Count
    /// outside the shadow, GBuffer and forward transparent passes.
    FormatPass CurrentFormatPass() const;
    /// A format's program for a pass, built on first use; null keeps the
    /// pass program, and skip is set when the format's program failed.
    OpenGLShader* GetFormatProgram(VertexFormatHandle format, FormatPass pass,
                                   bool& skip);
    /// Binds what the next draw needs: the format's program in the shadow,
    /// GBuffer and transparent passes, or the pass program back.
    void SelectFormatProgram(VertexFormatHandle format);
    /// Applies a format's cull mode; Default puts the pass state back.
    void ApplyFormatCull(VertexCullMode mode);
    /// Restores the cull state a format changed in this pass.
    void RestorePassCull();
    /// Forgets the program, cull and material switches of the last pass.
    void ResetFormatPassState();
    /// Deletes every custom-format program.
    void DestroyFormatPrograms();
    /// Enables or masks writes to GBuffer RT3.
    void SetMotionModelWrites(bool enabled);
    /// Binds the shadow material's diffuse at unit 0 when the bound shadow
    /// program samples it.
    void FlushShadowMaterial();

    /// Allocates the GBuffer FBO and its albedo/normal/metal-emit/depth attachments.
    bool CreateGBufferResources();
    void CleanupGBufferResources();
    /// Tears down and reallocates the GBuffer at a new swapchain size.
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

    /// Allocates the SSAO/blur FBOs, noise texture, and kernel/settings UBOs.
    bool CreateSSAOResources();
    void CleanupSSAOResources();
    /// Tears down and reallocates SSAO targets at a new swapchain size.
    void RecreateSSAOOnResize(int width, int height);
    /// Runs the SSAO sample pass followed by the blur pass into m_ssaoBlurTex.
    void ExecuteSSAOPass();

    // ---- IBL (Image-Based Lighting) ----
    class OpenGLIBL* m_ibl    = nullptr;
    GLuint m_iblSettingsUBO   = 0;   // binding 10 in lighting_pass_gl.frag
    GLuint m_iblBoundCubemap  = 0;   // last skybox cubemap we baked from
    /// Lazily (re)bakes IBL data when the bound skybox cubemap changes.
    void EnsureIBL();
    void CleanupIBL();

    // ---- Frame globals (sleak/binding.glsl SLEAK_*_GL) ----
    static constexpr GLuint FRAME_DATA_UBO_BINDING = 11;
    static constexpr GLuint FRAME_USER_UBO_BINDING = 12;
    static constexpr GLuint FRAME_CASCADE_UBO_BINDING = 13;
    static constexpr GLuint SHADOW_LIGHT_UBO_BINDING = 14;
    static constexpr GLuint FRAME_ATMOSPHERE_UBO_BINDING = 15;
    GLuint m_frameDataUBO = 0;
    GLuint m_frameUserUBO = 0;
    GLuint m_frameCascadeUBO = 0;
    GLuint m_shadowLightRawUBO = 0;  // ShadowLightUBO as Vulkan sees it
    GLuint m_frameAtmosphereUBO = 0;
    bool m_frameUBOsCreated = false;
    /// Creates and binds the frame, game, cascade, raw light and atmosphere
    /// buffers, zero-filled.
    bool CreateFrameUBOs();
    /// Deletes the buffers CreateFrameUBOs made.
    void CleanupFrameUBOs();

    // ---- Scene read for transparent formats ----
    static constexpr GLuint SCENE_COLOR_UNIT = 4;
    static constexpr GLuint SCENE_DEPTH_UNIT = 5;
    static constexpr GLuint SCENE_SKY_UNIT = 6;
    static constexpr GLuint SCENE_READ_UBO_BINDING = 4;
    GLuint m_sceneCopyTex = 0;  // RGBA8 snapshot of the default framebuffer
    int m_sceneCopyWidth = 0;
    int m_sceneCopyHeight = 0;
    GLuint m_sceneReadUBO = 0;
    GLuint m_sceneFallbackCube = 0;
    GLuint m_sceneSkyCube = 0;  // skybox cubemap found this frame, 0 if none
    bool m_sceneReadPrepared = false;  // snapshot taken this frame
    float m_cachedInvViewProj[16] = {};
    float m_cachedNearPlane = 0.0f;
    float m_cachedFarPlane = 0.0f;
    /// Returns the active scene's skybox cubemap, or 0 when there is none.
    GLuint FindSkyboxCubemap() const;
    /// Snapshots the scene once per frame on the first scene-reading draw
    /// and binds the snapshot, depth, sky, and SceneReadUBO.
    void PrepareSceneRead();
    /// Deletes the snapshot texture, stand-in cubemap, and UBO.
    void CleanupSceneRead();
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _OPENGLRENDERER_H
