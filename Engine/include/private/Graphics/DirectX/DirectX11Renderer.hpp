#include <Core/OSDef.hpp>

#ifdef PLATFORM_WIN

#ifndef _DIRECTX11RENDERER_H
#define _DIRECTX11RENDERER_H

#include "Graphics/RenderContext.hpp"
#include "Graphics/Renderer.hpp"
#include "Graphics/ResourceManager.hpp"
#include <Utility/Container/List.hpp>
#include <d3d11.h>
#include <Window.hpp>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <chrono>
#include <Core/Timer.hpp>
#include <Utility/Container/Queue.hpp>

namespace Sleak {
namespace RenderEngine {

class ENGINE_API DirectX11Renderer : public Renderer, public RenderContext {
public:
    DirectX11Renderer(Window* window);
    ~DirectX11Renderer() override;

    bool Initialize() override;
    void BeginRender() override;
    void EndRender() override;
    void Cleanup() override;

    bool CreateDepthStencilBuffer(uint32_t width, uint32_t height);
    void SetDepthStencilState(bool enableDepth, bool enableStencil);

    virtual void Resize(uint32_t width, uint32_t height) override;

    virtual void Draw(uint32_t vertexCount) override;
    virtual void DrawIndexed(uint32_t indexCount) override;
    virtual void DrawInstance(uint32_t instanceCount, uint32_t vertexPerInstance) override;
    virtual void DrawIndexedInstance(uint32_t instanceCount, uint32_t indexPerInstance) override;

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

    Texture* CreateCubemapTexture(const std::array<std::string, 6>& facePaths);
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

    virtual RenderContext* GetContext() override { return this; }

   private:
    virtual void ConfigureRenderMode() override;
    virtual void ConfigureRenderFace() override;
    bool CreateRenderTargetView();
    bool SetRasterState();

    bool CreateBlendState();
    void SetBlendState(float r, float g, float b, float a, UINT Mask);

    void CheckMSAASupport();
    bool CreateMSAARenderTarget();
    bool CreateMSAADepthStencil();
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

    ID3D11DepthStencilState* depthStencilState;
    ID3D11DepthStencilView* depthStencilView;
    ID3D11Texture2D* depthStencilBuffer;

    ID3D11BlendState* blendState;

    ID3D11Texture2D* msaaRenderTarget = nullptr;
    ID3D11RenderTargetView* msaaRenderTargetView = nullptr;
    ID3D11Texture2D* msaaDepthStencilBuffer = nullptr;
    ID3D11DepthStencilView* msaaDepthStencilView = nullptr;
    
    ID3D11Query* query;

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

    ImGuiContext* ImCon;
    Timer FrameTimer;
    std::chrono::steady_clock::time_point LastFrame;
    Queue<float> FrameTimes;

};

} // namespace RenderEngine
} // namespace Sleak

#endif // _DIRECTX11RENDERER_H

#endif
