#include <Core/OSDef.hpp>

#ifdef PLATFORM_WIN

#ifndef _DIRECTX12RENDERER_H
#define _DIRECTX12RENDERER_H

#include "Graphics/Common/Renderer.hpp"
#include "Graphics/Common/RenderContext.hpp"
#include "Graphics/Common/ResourceManager.hpp"
#include "Graphics/DirectX12/DirectX12Buffer.hpp"
#include "Graphics/DirectX12/DirectX12Shader.hpp"
#include "Graphics/DirectX12/DirectX12Texture.hpp"
#include "Graphics/DirectX12/DirectX12UploadContext.hpp"
#include "Graphics/DirectX12/DirectX12UploadRing.hpp"
#include <Core/Window.hpp>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <utility>
#include <vector>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>

namespace Sleak {
namespace RenderEngine {

/// D3D12 backend: explicit command lists, per-frame allocators, and a shared SRV descriptor heap.
class ENGINE_API DirectX12Renderer : public Renderer, public RenderContext {
public:
    DirectX12Renderer(Window* window);
    virtual ~DirectX12Renderer();

    bool Initialize() override;
    void BeginRender() override;
    void EndRender() override;
    void Cleanup() override;
    void WaitIdle() override { WaitForGPU(); }

    virtual void Resize(uint32_t width, uint32_t height) override;

    /// True if the current adapter/driver supports D3D12 feature level 11_0.
    static bool IsSupport();

    virtual bool CreateImGUI() override;

    virtual RenderContext* GetContext() override { return this; }

    // Feature capability mask
    virtual uint32_t GetFeatureCaps() const override { return CapShadows; }

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

    virtual void BindVertexBuffer(RefPtr<BufferBase> buffer,
                                  uint32_t slot = 0) override;
    virtual void BindIndexBuffer(RefPtr<BufferBase> buffer,
                                 uint32_t slot = 0) override;
    virtual void BindConstantBuffer(RefPtr<BufferBase> buffer,
                                    uint32_t slot = 0) override;

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

    // Lighting/fog UBO (matches ShadowLightUBO from Vulkan path)
    void UpdateShadowLightUBO(const void* data, uint32_t size) override;

    // Skybox state management
    virtual void BindTexture(RefPtr<Sleak::Texture> texture, uint32_t slot = 0) override;
    virtual void BindTextureRaw(Sleak::Texture* texture, uint32_t slot = 0) override;
    virtual void BeginSkyboxPass() override;
    virtual void EndSkyboxPass() override;
    virtual void BeginDebugLinePass() override;
    virtual void EndDebugLinePass() override;

    /// Sets a shader's PSO unless a skybox or debug line pass owns it.
    void BindShaderPipeline(ID3D12PipelineState* pso);

private:
    /// Fixed-function state that, with the shader bytecode, identifies a PSO.
    struct PipelineKey {
        uint64_t vsHash = 0;
        uint64_t psHash = 0;
        uint32_t vertexFormat = 0;
        D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_FRONT;
        BOOL depthClip = TRUE;
        INT depthBias = 0;
        float depthBiasClamp = 0.0f;
        float slopeScaledDepthBias = 0.0f;
        D3D12_DEPTH_WRITE_MASK depthWrite = D3D12_DEPTH_WRITE_MASK_ALL;
        D3D12_COMPARISON_FUNC depthFunc = D3D12_COMPARISON_FUNC_LESS;
        BOOL blendEnable = FALSE;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topology =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT;

        bool operator==(const PipelineKey&) const = default;
    };

    /// Returns the cached PSO for the key, building it on a miss.
    ID3D12PipelineState* GetPipeline(const PipelineKey& key, ID3DBlob* vs,
                                     ID3DBlob* ps);
    /// Reapplies the last shader PSO after a pass override ends.
    void RestoreShaderPipeline();
    /// Copies the light/fog constants into the ring and binds them at b2.
    void BindLightConstants();

    /// Creates the DXGI factory, enumerates adapters, and creates the D3D12 device.
    bool CreateDevice();
    bool CreateCommandQueue();
    /// Creates the swapchain and its per-frame buffers.
    bool CreateSwapChain();
    /// Creates per-frame command allocators and the shared graphics command list.
    bool CreateCommandAllocatorAndList();
    /// Creates the RTV heap and a render target view for each swapchain buffer.
    bool CreateRenderTargetViews();
    bool CreateDepthStencilView();
    bool CreateFence();
    bool CreateRootSignature();

    virtual void ConfigureRenderMode() override;
    virtual void ConfigureRenderFace() override;

    /// Signals the fence and blocks the CPU until the GPU catches up.
    void WaitForGPU();
    /// Logs available DXGI adapters and picks the current one for device creation.
    void EnumerateDevices(Microsoft::WRL::ComPtr<IDXGIFactory4> factory);

    // Frame count (must be declared before arrays that use it)
    static constexpr UINT FrameCount = 2;

    // Device and swap chain
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;

    // Command objects (per-frame allocators for CPU/GPU overlap)
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocators[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;

    // Descriptor heaps
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;

    // Render targets and depth
    Microsoft::WRL::ComPtr<ID3D12Resource> renderTargets[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencilBuffer;

    // Pipeline state (PSOs are owned by the cache)
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    std::vector<std::pair<PipelineKey,
                          Microsoft::WRL::ComPtr<ID3D12PipelineState>>>
        m_pipelineCache;
    ID3D12PipelineState* m_defaultPipelineState = nullptr;
    ID3D12PipelineState* m_shaderPipeline = nullptr;
    ID3D12PipelineState* m_passPipeline = nullptr;

    // Synchronization: one monotonically increasing fence, and the value
    // that marks the end of the last frame recorded into each slot
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    UINT64 fenceValues[FrameCount] = {};
    uint64_t m_frameSerial = 0;
    bool m_frameActive = false;

    // Descriptor sizes
    UINT rtvDescriptorSize = 0;
    UINT frameIndex = 0;

    // Window
    Window* window;

    // Viewport and scissor rect
    D3D12_VIEWPORT viewport = {};
    D3D12_RECT scissorRect = {};

    bool m_Initialized = false;

    // Default 1x1 white texture (fallback when no texture is bound)
    DirectX12Texture* m_defaultTexture = nullptr;

    // Skybox PSO (depth write off, LEQUAL, no cull)
    ID3D12PipelineState* m_skyboxPipelineState = nullptr;
    bool m_skyboxPipelineFailed = false;
    /// Builds the skybox PSO (depth write disabled, LEQUAL compare, no culling).
    bool CreateSkyboxPipelineState();

    // Debug line PSO (line topology)
    ID3D12PipelineState* m_debugLinePipelineState = nullptr;
    bool m_debugLinePipelineFailed = false;
    /// Builds the line-topology PSO used for debug line rendering.
    bool CreateDebugLinePipelineState();

    // Transient per-frame constant data (constant buffers, light/fog)
    DirectX12UploadRing m_uploadRing;
    // Batched init-time and dynamic buffer/texture uploads
    DirectX12UploadContext m_uploader;

    // Latest light/fog constants, copied into the ring when bound
    std::vector<uint8_t> m_lightData;

    // Shadow mapping
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_shadowDepthBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_shadowDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE                    m_shadowDsvHandle = {};
    UINT                                           m_shadowSrvIndex = 0;
    bool                                           m_shadowMapCreated = false;
    float                                          m_lightVP[16] = {};
    float                                          m_pendingLightVP[16] = {};
    bool                                           m_hasPendingLightVP = false;
    bool                                           m_inShadowPass = false;
    // Depth-only PSO for shadow pass (no PS, no RTV, CULL_NONE)
    ID3D12PipelineState*                           m_shadowPassPSO = nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob>               m_shadowVSBlob;
    bool                                           m_shadowVSCompiled = false;
    // First shader VS, fallback for the shadow PSO
    Microsoft::WRL::ComPtr<ID3DBlob>               m_cachedVSBlob;
    void SetLightVP(const float* mat) override;
    /// Allocates the shadow-pass depth buffer, DSV, and shared-heap SRV slot.
    bool CreateShadowMapResources();
    /// Builds the depth-only PSO (no pixel shader, no RTV, no culling) used for the shadow pass.
    bool CreateShadowPassPSO();
    /// Replays cached shadow-caster draws into the depth-only shadow map.
    void RenderShadowPass();

    // ImGUI
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> imguiSrvHeap;

    // Shared SRV descriptor heap (all textures live here — set once per frame)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_sharedSrvHeap;
    UINT m_srvDescriptorSize = 0;
    UINT m_nextSrvSlot = 1; // slot 0 reserved for imgui/default
    static constexpr UINT MAX_SRV_DESCRIPTORS = 512;

    /// Creates the shared, shader-visible SRV descriptor heap used by all textures/cubemaps.
    bool CreateSharedSrvHeap();
public:
    // Allocate a slot in the shared SRV heap; returns the slot index
    UINT AllocateSRVSlot();
    D3D12_CPU_DESCRIPTOR_HANDLE GetSharedSrvCPUHandle(UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSharedSrvGPUHandle(UINT slot) const;
    ID3D12DescriptorHeap* GetSharedSrvHeap() const { return m_sharedSrvHeap.Get(); }
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _DIRECTX12RENDERER_H

#else
namespace Sleak::RenderEngine {
class DirectX12Renderer;
}

#endif
