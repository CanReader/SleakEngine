#include <Core/OSDef.hpp>

#ifdef PLATFORM_WIN

#ifndef _DIRECTX11RENDERER_H
#define _DIRECTX11RENDERER_H

#include <backends/imgui_impl_dx11.h>
#include <d3d11.h>
#include <imgui.h>
#include <wrl/client.h>

#include <Core/Timer.hpp>
#include <Core/Window.hpp>
#include <Utility/Container/List.hpp>
#include <Utility/Container/Queue.hpp>
#include <chrono>
#include <utility>
#include <vector>

#include "Graphics/Common/RenderContext.hpp"
#include "Graphics/Common/Renderer.hpp"
#include "Graphics/Common/ResourceManager.hpp"

namespace Sleak {
namespace RenderEngine {

/// D3D11 backend: forward pipeline with shadow mapping, MSAA, and ACES tonemapping.
class ENGINE_API DirectX11Renderer : public Renderer, public RenderContext {
public:
    DirectX11Renderer(Window* window);
    ~DirectX11Renderer() override;

    bool Initialize() override;
    void BeginRender() override;
    void EndRender() override;
    void Cleanup() override;
#ifdef SLEAK_HOT_RELOAD
    /// Recompiles every live shader and fixed pass built from one of the stems.
    bool ReloadShaders(const std::vector<std::string>& stems) override;
#endif

    /// Allocates the depth/stencil texture and view sized to the swapchain.
    bool CreateDepthStencilBuffer(uint32_t width, uint32_t height);
    /// Creates and binds a depth-stencil state matching the requested depth/stencil toggles.
    void SetDepthStencilState(bool enableDepth, bool enableStencil);

    virtual void Resize(uint32_t width, uint32_t height) override;

    // Feature capability mask
    virtual uint32_t GetFeatureCaps() const override {
        return CapShadows | CapTonemapPass;
    }

    virtual void Draw(uint32_t vertexCount) override;
    virtual void Draw(uint32_t vertexCount, uint32_t firstVertex) override;
    virtual void DrawIndexed(uint32_t indexCount) override;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t firstIndex,
                             int32_t baseVertex) override;
    virtual void DrawInstance(uint32_t instanceCount, uint32_t vertexPerInstance) override;
    virtual void DrawIndexedInstance(uint32_t instanceCount, uint32_t indexPerInstance) override;
    virtual void DrawIndexedInstance(uint32_t instanceCount,
                                     uint32_t indexPerInstance,
                                     uint32_t firstIndex,
                                     int32_t baseVertex) override;

    // State management
    virtual void SetRenderMode(RenderMode mode) override;
    virtual void SetRenderFace(RenderFace face) override;
    virtual void SetViewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f) override;
    virtual void ClearRenderTarget(float r, float g, float b, float a) override;
    virtual void ClearDepthStencil(bool clearDepth, bool clearStencil,
                                   float depth, uint8_t stencil) override;

    // Buffer binding
    virtual void BindVertexBuffer(RefPtr<BufferBase> buffer,
                                  uint32_t slot = 0) override;
    virtual void BindIndexBuffer(RefPtr<BufferBase> buffer,
                                 uint32_t slot = 0) override;
    virtual void BindConstantBuffer(RefPtr<BufferBase> buffer,
                                    uint32_t slot = 0) override;

    virtual BufferBase* CreateBuffer(BufferType Type, uint32_t size, void* data) override;
    virtual Shader* CreateShader(const std::string& shaderSource) override;
    virtual Texture* CreateTexture(const std::string& TexturePath) override;
    virtual Texture* CreateTextureFromData(uint32_t width, uint32_t height,
                                           void* data) override;

    /// Loads a cubemap from six face image paths.
    Texture* CreateCubemapTexture(const std::array<std::string, 6>& facePaths);
    /// Loads a cubemap by converting a single equirectangular panorama.
    Texture* CreateCubemapTextureFromPanorama(const std::string& panoramaPath);

    // Skybox state management
    virtual void SetDepthWrite(bool enabled) override;
    virtual void SetDepthCompare(DepthCompare compare) override;
    virtual void SetCullEnabled(bool enabled) override;
    virtual void BindTexture(RefPtr<Sleak::Texture> texture, uint32_t slot = 0) override;
    virtual void BeginSkyboxPass() override;
    virtual void EndSkyboxPass() override;
    virtual void BeginDebugLinePass() override;
    virtual void EndDebugLinePass() override;

    virtual bool CreateImGUI();

    void ApplyMSAAChange() override;

    // Shadow mapping support
    void UpdateShadowLightUBO(const void* data, uint32_t size) override;

    virtual RenderContext* GetContext() override { return this; }

   private:
    virtual void ConfigureRenderMode() override;
    virtual void ConfigureRenderFace() override;
    /// Creates the back-buffer render target view from the swapchain.
    bool CreateRenderTargetView();
    /// Creates and binds the rasterizer state matching current fill/cull settings.
    bool SetRasterState();
    /// Binds the depth state matching the tracked depth write and compare.
    void ApplyDepthState();
    /// Returns the cached rasterizer state for desc, creating it on a miss.
    ID3D11RasterizerState* GetRasterState(const D3D11_RASTERIZER_DESC& desc);
    /// Returns the cached depth-stencil state for desc, creating it on a miss.
    ID3D11DepthStencilState* GetDepthState(
        const D3D11_DEPTH_STENCIL_DESC& desc);

    /// Creates the alpha-blend state used for transparent draws.
    bool CreateBlendState();
    void SetBlendState(float r, float g, float b, float a, UINT Mask);

    /// Queries the device for supported MSAA sample counts and their quality levels.
    void CheckMSAASupport();
    /// Allocates the multisampled color render target at the current sample count.
    bool CreateMSAARenderTarget();
    /// Allocates the multisampled depth-stencil buffer at the current sample count.
    bool CreateMSAADepthStencil();
    /// Resolves the multisampled render target into the single-sample back buffer.
    void ResolveMSAA();

    
    bool bIsLayoutCreated = false;

    List<std::string> SupportedMSAA;

    UINT msaaSampleCount = 1;
    UINT msaaQualityLevel = 0; 

    IDXGISwapChain* swapChain;           // Swap chain for double buffering

    ID3D11Device* device;                // DirectX 11 device
    ID3D11DeviceContext* deviceContext;  // DirectX 11 device context
    ID3D11RenderTargetView* renderTargetView; // Render target view
    ID3D11InputLayout* layout;

    ID3D11DepthStencilState* depthStencilState = nullptr;
    ID3D11DepthStencilView* depthStencilView;
    ID3D11Texture2D* depthStencilBuffer;

    ID3D11BlendState* blendState;

    ID3D11Texture2D* msaaRenderTarget = nullptr;
    ID3D11RenderTargetView* msaaRenderTargetView = nullptr;
    ID3D11Texture2D* msaaDepthStencilBuffer = nullptr;
    ID3D11DepthStencilView* msaaDepthStencilView = nullptr;

    ID3D11Query* query = nullptr;

    D3D11_PRIMITIVE_TOPOLOGY topology;
    D3D11_FILL_MODE fillMode;
    D3D11_CULL_MODE cull;
    Window* window;

    // Skybox state
    D3D11_PRIMITIVE_TOPOLOGY m_savedTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    ID3D11DepthStencilState* m_savedDepthStencilState = nullptr;
    D3D11_CULL_MODE m_savedCullMode = D3D11_CULL_FRONT;
    bool m_depthWriteEnabled = true;
    D3D11_COMPARISON_FUNC m_depthFunc = D3D11_COMPARISON_LESS;

    // Immutable state objects keyed by description, owned until Cleanup
    std::vector<std::pair<D3D11_RASTERIZER_DESC,
                          Microsoft::WRL::ComPtr<ID3D11RasterizerState>>>
        m_rasterStates;
    std::vector<std::pair<D3D11_DEPTH_STENCIL_DESC,
                          Microsoft::WRL::ComPtr<ID3D11DepthStencilState>>>
        m_depthStates;

    ImGuiContext* ImCon;

    // Shadow mapping
    ID3D11Buffer* m_shadowCB = nullptr;
    bool m_shadowCBCreated = false;
    /// Allocates the constant buffer backing PCSSShadowGPUData.
    bool CreateShadowConstantBuffer();
    void CleanupShadowResources();

    // Shadow depth map
    ID3D11Texture2D*          m_shadowDepthTex = nullptr;
    ID3D11DepthStencilView*   m_shadowDSV = nullptr;
    ID3D11ShaderResourceView* m_shadowSRV = nullptr;
    ID3D11SamplerState*       m_shadowSampler = nullptr;
    ID3D11RasterizerState*    m_shadowRasterState = nullptr;
    bool m_shadowMapCreated = false;
    float m_lightVP[16] = {};
    float m_pendingLightVP[16] = {};
    bool  m_hasPendingLightVP = false;
    ID3D11Buffer* m_shadowTransformCB = nullptr;  // Dedicated CB for shadow pass transforms
    // World last uploaded to m_shadowTransformCB, valid until lightVP changes
    float m_shadowWorldCache[16] = {};
    bool m_shadowCBCacheValid = false;
    bool m_inShadowPass = false;
    void SetLightVP(const float* mat) override;
    /// Allocates the shadow-pass depth texture, DSV/SRV, and comparison sampler.
    bool CreateShadowMapResources();
    /// Replays cached shadow-caster draws into the depth-only shadow map.
    void RenderShadowPass();

    // Post-process: Tonemapping
    ID3D11Buffer* m_postProcessCB = nullptr;
    ID3D11Texture2D* m_hdrTexture = nullptr;
    ID3D11RenderTargetView* m_hdrRTV = nullptr;
    ID3D11ShaderResourceView* m_hdrSRV = nullptr;
    ID3D11SamplerState* m_pointSampler = nullptr;
    ID3D11VertexShader* m_tonemapVS = nullptr;
    ID3D11PixelShader* m_tonemapPS = nullptr;
    bool m_tonemapResourcesCreated = false;
    /// Allocates the HDR offscreen target and tonemap shaders/sampler.
    bool CreateTonemapResources();
    /// Compiles tonemap.hlsl, writing the outputs only on success.
    bool CompileTonemapShaders(ID3D11VertexShader** vs, ID3D11PixelShader** ps);
#ifdef SLEAK_HOT_RELOAD
    /// Swaps in recompiled tonemap shaders, keeping the old ones on failure.
    void ReloadTonemapShaders();
#endif
    void CleanupTonemapResources();
    /// Runs the ACES tonemap full-screen pass from the HDR target to the back buffer.
    void ExecuteTonemapPass();
};

} // namespace RenderEngine
} // namespace Sleak

#endif // _DIRECTX11RENDERER_H

#endif
