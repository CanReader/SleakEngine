#include <Core/OSDef.hpp>

#ifdef PLATFORM_WIN

#ifndef _DIRECTX12RENDERER_H
#define _DIRECTX12RENDERER_H

#include "Graphics/Renderer.hpp"
#include "Graphics/RenderContext.hpp"
#include "Graphics/ResourceManager.hpp"
#include "Graphics/DirectX/DirectX12Buffer.hpp"
#include "Graphics/DirectX/DirectX12Shader.hpp"
#include "Graphics/DirectX/DirectX12Texture.hpp"
#include <Window.hpp>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>

namespace Sleak {
namespace RenderEngine {

class ENGINE_API DirectX12Renderer : public Renderer, public RenderContext {
public:
    DirectX12Renderer(Window* window);
    virtual ~DirectX12Renderer();

    bool Initialize() override;
    void BeginRender() override;
    void EndRender() override;
    void Cleanup() override;

    virtual void Resize(uint32_t width, uint32_t height) override;

    static bool IsSupport();

    virtual bool CreateImGUI() override;

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

private:
    bool CreateDevice();
    bool CreateCommandQueue();
    bool CreateSwapChain();
    bool CreateCommandAllocatorAndList();
    bool CreateRenderTargetViews();
    bool CreateDepthStencilView();
    bool CreateFence();
    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreatePipelineStateFromShader(ID3DBlob* vertexShaderBlob,
                                       ID3DBlob* pixelShaderBlob);

    virtual void ConfigureRenderMode() override;
    virtual void ConfigureRenderFace() override;

    void WaitForGPU();
    void EnumerateDevices(Microsoft::WRL::ComPtr<IDXGIFactory4> factory);

    // Device and swap chain
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;

    // Command objects
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;

    // Descriptor heaps
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;

    // Render targets and depth
    static constexpr UINT FrameCount = 2;
    Microsoft::WRL::ComPtr<ID3D12Resource> renderTargets[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencilBuffer;

    // Pipeline state
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;

    // Synchronization
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;

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

    // ImGUI
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> imguiSrvHeap;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _DIRECTX12RENDERER_H

#else
namespace Sleak::RenderEngine {
class DirectX12Renderer;
}

#endif
