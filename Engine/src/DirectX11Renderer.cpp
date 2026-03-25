#include "../../include/private/Graphics/DirectX/DirectX11Renderer.hpp"
#ifdef PLATFORM_WIN

#include <Window.hpp>
#include <SDL3/SDL.h>
#include <dxgidebug.h>
#include <Graphics/DirectX/DirectX11Buffer.hpp>
#include <Graphics/DirectX/DirectX11Shader.hpp>
#include "Graphics/DirectX/DirectX11Texture.hpp"
#include "Graphics/DirectX/DirectX11CubemapTexture.hpp"
#include "Graphics/Vertex.hpp"
#include <Graphics/ConstantBuffer.hpp>
#include <imgui_internal.h>
#include <windows.h>
#include <psapi.h>
#include <d3dcompiler.h>
#include <cstring>
#include "Graphics/RenderCommandQueue.hpp"

namespace Sleak {
    namespace RenderEngine {
        
DirectX11Renderer::DirectX11Renderer(Window* window) 
    : window(window),
      device(nullptr),
      deviceContext(nullptr),
      swapChain(nullptr),
      renderTargetView(nullptr) {
        this->Type = RendererType::DirectX11;
        this->cull = D3D11_CULL_FRONT;

        ResourceManager::RegisterCreateBuffer(this,&DirectX11Renderer::CreateBuffer);
        ResourceManager::RegisterCreateShader(this,&DirectX11Renderer::CreateShader);
        ResourceManager::RegisterCreateTexture(this,&DirectX11Renderer::CreateTexture);
        ResourceManager::RegisterCreateCubemapTexture(this,&DirectX11Renderer::CreateCubemapTexture);
        ResourceManager::RegisterCreateCubemapTextureFromPanorama(this,&DirectX11Renderer::CreateCubemapTextureFromPanorama);
        ResourceManager::RegisterCreateTextureFromMemory(
            [this](const void* data, uint32_t w, uint32_t h, TextureFormat fmt) -> Texture* {
                auto* tex = new DirectX11Texture(device);
                if (tex->LoadFromMemory(data, w, h, fmt)) return tex;
                delete tex;
                return nullptr;
            });

      }

DirectX11Renderer::~DirectX11Renderer() {
    Cleanup();
}

bool DirectX11Renderer::Initialize() {
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window->GetSDLWindow()),
                                     SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    // Create a DirectX 11 device and swap chain
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Width = Window::GetWidth();
    swapChainDesc.BufferDesc.Height = Window::GetHeight();
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT createDeviceFlags = 0;
    #ifdef _DEBUG
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    #endif

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,                        // Use default adapter
        D3D_DRIVER_TYPE_HARDWARE,       // Use hardware rendering
        nullptr,                        // No software device
        createDeviceFlags,              // No creation flags
        featureLevels,                  // Feature levels
        1,                              // Number of feature levels
        D3D11_SDK_VERSION,              // SDK version
        &swapChainDesc,                 // Swap chain description
        &swapChain,                     // Swap chain
        &device,                        // Device
        nullptr,                        // Supported feature level
        &deviceContext                  // Device context
    );

    if (FAILED(hr)) {
        return false;
    }

    CheckMSAASupport();

   if (!CreateRenderTargetView() ||
        !CreateDepthStencilBuffer(window->GetWidth(), window->GetHeight())) {
        return false;
    }

    SetViewport(0, 0, window->GetWidth(), window->GetHeight());

    SLEAK_INFO("DirectX 11 has successfully created!");

    #ifdef _DEBUG
        ID3D11InfoQueue* infoQueue;
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&infoQueue))) {
            // Only break on corruption (critical), not on errors — breaking on
            // errors without a debugger attached silently drops draw calls.
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);

            D3D11_MESSAGE_SEVERITY severities[] = {
                D3D11_MESSAGE_SEVERITY_INFO
            };

            D3D11_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumSeverities = _countof(severities);
            filter.DenyList.pSeverityList = severities;

            infoQueue->AddStorageFilterEntries(&filter);
            infoQueue->Release();
        }
    #endif

        SetPerformanceCounter(true);
        SetRenderMode(RenderMode::Fill);
        SetRenderFace(RenderFace::Front);

        ConfigureRenderMode();

        SetDepthStencilState(true, true);

        msaaSampleCount = 1;

        D3D11_QUERY_DESC queryDesc;
        queryDesc.Query = D3D11_QUERY::D3D11_QUERY_PIPELINE_STATISTICS;
        queryDesc.MiscFlags = 0;
        if(FAILED(device->CreateQuery(&queryDesc,&query)))
            SLEAK_WARN("Could not create pipeline statics object!");
        
            
    if(bEnabledPerformanceCounter){
        if(query)
        while (deviceContext->GetData(query, nullptr, 0, 0) == S_FALSE);
        deviceContext->Begin(query);
    }

        if (!CreateBlendState()) 
            return false;

    return true;
}

void DirectX11Renderer::BeginRender() {
    if (m_msaaChangeRequested)
        ApplyMSAAChange();

    // Create tonemap resources on demand
    if (m_tonemapEnabled && !m_tonemapResourcesCreated)
        CreateTonemapResources();

    // When tonemapping is enabled and no MSAA, render to HDR target
    if (m_tonemapEnabled && m_tonemapResourcesCreated && msaaSampleCount <= 1) {
        deviceContext->OMSetRenderTargets(1, &m_hdrRTV, depthStencilView);
    } else if (msaaSampleCount > 1 && msaaRenderTargetView && msaaDepthStencilView) {
        deviceContext->OMSetRenderTargets(1, &msaaRenderTargetView, msaaDepthStencilView);
    } else {
        deviceContext->OMSetRenderTargets(1, &renderTargetView, depthStencilView);
    }

    ClearRenderTarget(0.39f, 0.58f, 0.93f, 1.0f);
    ClearDepthStencil(true, false, 1.0, 0);

    // Shadow pass: render shadow depth map before main pass
    if (m_shadowPassEnabled) {
        RenderShadowPass();
    }

    // Ensure shadow map SRV + comparison sampler stay bound at t3/s3
    // for the main draw calls (guards against any intermediate state
    // clearing the slots between shadow pass and ExecuteCommands).
    if (m_shadowMapCreated && m_shadowSRV && m_shadowSampler) {
        deviceContext->PSSetShaderResources(3, 1, &m_shadowSRV);
        deviceContext->PSSetSamplers(3, 1, &m_shadowSampler);
    }

    if (bIsLayoutCreated)
        deviceContext->IASetInputLayout(layout);

    if(bImInitialized) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
    }
}

void DirectX11Renderer::EndRender() {
    // Resolve MSAA before presenting
    if (msaaSampleCount > 1) {
        // Switch back to default render target for ImGui
        deviceContext->OMSetRenderTargets(1, &renderTargetView, depthStencilView);
        ResolveMSAA();
    }

    // Run tonemapping post-process pass (HDR -> backbuffer)
    if (m_tonemapEnabled && m_tonemapResourcesCreated && msaaSampleCount <= 1) {
        ExecuteTonemapPass();
        // Restore input layout after fullscreen pass
        if (bIsLayoutCreated)
            deviceContext->IASetInputLayout(layout);
    }

    // ImGui renders on top of tonemapped output
    if (bImInitialized) {
        // Ensure we're rendering to the backbuffer
        deviceContext->OMSetRenderTargets(1, &renderTargetView, nullptr);
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    swapChain->Present(m_vsync ? 1 : 0, 0);

    UpdateFrameMetrics();
}

void DirectX11Renderer::Cleanup() {
    bImInitialized = false;

    // Cleanup post-process resources
    CleanupTonemapResources();
    CleanupShadowResources();

    // Cleanup MSAA resources
    if (msaaRenderTarget) { msaaRenderTarget->Release(); msaaRenderTarget = nullptr; }
    if (msaaRenderTargetView) { msaaRenderTargetView->Release(); msaaRenderTargetView = nullptr; }
    if (msaaDepthStencilBuffer) { msaaDepthStencilBuffer->Release(); msaaDepthStencilBuffer = nullptr; }
    if (msaaDepthStencilView) { msaaDepthStencilView->Release(); msaaDepthStencilView = nullptr; }

    if (renderTargetView) {
        renderTargetView->Release();
        renderTargetView = nullptr;
    }
    if (swapChain) {
        swapChain->Release();
        swapChain = nullptr;
    }
    if (deviceContext) {
        deviceContext->Release();
        deviceContext = nullptr;
    }

    if (depthStencilView) {
        depthStencilView->Release();
        depthStencilView = nullptr;
    }
    if (depthStencilBuffer) {
        depthStencilBuffer->Release();
        depthStencilBuffer = nullptr;
    }
    if (depthStencilState) {
        depthStencilState->Release();
        depthStencilState = nullptr;
    }

    if (blendState) {
        blendState->Release();
        blendState = nullptr;
    }

    if(query)
    {
        query->Release();
        query = nullptr;
    }
    
    if (bImInitialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        bImInitialized = false;
    }

    #ifdef _DEBUG
        if(device) {
            ID3D11Debug* debugDevice;
            device->QueryInterface(__uuidof(ID3D11Debug), (void**)&debugDevice);
            if (debugDevice) {
                debugDevice->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
                debugDevice->Release();
            }
        }
    #endif

    if (device) {
        device->Release();
        device = nullptr;
    }
}

bool DirectX11Renderer::CreateDepthStencilBuffer(uint32_t width,uint32_t height) {

    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr =
        device->CreateTexture2D(&depthDesc, nullptr, &depthStencilBuffer);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create depth stencil buffer!");
        return false;
    }

    hr = device->CreateDepthStencilView(depthStencilBuffer, nullptr,
                                        &depthStencilView);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create depth stencil view!");
        return false;
    }

    deviceContext->OMSetRenderTargets(1, &renderTargetView, depthStencilView);
    return true;
}

void DirectX11Renderer::SetDepthStencilState(bool enableDepth,
                                             bool enableStencil) {
    D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = enableDepth;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
    depthStencilDesc.StencilEnable = enableStencil;
    depthStencilDesc.StencilReadMask = 0xFF;
    depthStencilDesc.StencilWriteMask = 0xFF;

    // Front face stencil operations
    depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
    depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

    // Back face stencil operations
    depthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
    depthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

    HRESULT hr = device->CreateDepthStencilState(&depthStencilDesc, &depthStencilState);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create depth stencil state!");
        return;
    }

    deviceContext->OMSetDepthStencilState(depthStencilState, 1);
}

void DirectX11Renderer::Resize(uint32_t width, uint32_t height) {
    if (swapChain == nullptr || device == nullptr || deviceContext == nullptr) {
        return;
    }
    if (width == 0 || height == 0) return;

    // Unbind all pipeline resources so DXGI can release the back-buffer references
    deviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    deviceContext->ClearState();
    deviceContext->Flush();

    // Release all references to the swap chain's buffers
    if (renderTargetView) {
        renderTargetView->Release();
        renderTargetView = nullptr;
    }

    if (depthStencilView) {
        depthStencilView->Release();
        depthStencilView = nullptr;
    }
    if (depthStencilBuffer) {
        depthStencilBuffer->Release();
        depthStencilBuffer = nullptr;
    }
    if (depthStencilState) {
        depthStencilState->Release();
        depthStencilState = nullptr;
    }

    // Use 0 to preserve the existing buffer count (FLIP_DISCARD requires >=2)
    // Use DXGI_FORMAT_UNKNOWN to preserve the existing format
    HRESULT hr = swapChain->ResizeBuffers(0,
                                          width, height,
                                          DXGI_FORMAT_UNKNOWN,
                                          0);

    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to resize swap chain buffers! HRESULT: 0x{:X}", (unsigned)hr);
        return;
    }

    // Recreate the render target view
    if (!CreateRenderTargetView() || !CreateDepthStencilBuffer(width, height)) {
        return;
    }

    // Recreate MSAA targets if MSAA is active
    if (msaaSampleCount > 1) {
        CreateMSAARenderTarget();
        CreateMSAADepthStencil();
    }

    // Recreate HDR tonemap target at new size
    if (m_tonemapResourcesCreated) {
        CleanupTonemapResources();
        CreateTonemapResources();
    }

    // Update the viewport
    SetViewport(0, 0, width, height);

    ImCon->IO.DisplaySize = ImVec2(width,height);

    SLEAK_INFO("DirectX 11 resized to {}x{}", width, height);
}

void DirectX11Renderer::Draw(uint32_t vertexCount) {
    deviceContext->Draw(vertexCount,0);
}

void DirectX11Renderer::DrawIndexed(uint32_t indexCount) {
    deviceContext->DrawIndexed(indexCount, 0, 0);
    DrawnVertices += indexCount;
}
    
void DirectX11Renderer::DrawInstance(uint32_t instanceCount,
                                     uint32_t vertexPerInstance) {
    deviceContext->DrawInstanced(vertexPerInstance, instanceCount, 0, 0);
}

void DirectX11Renderer::DrawIndexedInstance(uint32_t instanceCount,
                                            uint32_t indexPerInstance) {
    deviceContext->DrawIndexedInstanced(indexPerInstance, instanceCount, 0, 0, 0);
}

void DirectX11Renderer::ClearRenderTarget(float r, float g, float b, float a) {
    const float clearColor[4] = {r, g, b, a};

    ID3D11RenderTargetView* target;
    if (m_tonemapEnabled && m_tonemapResourcesCreated && msaaSampleCount <= 1)
        target = m_hdrRTV;
    else if (msaaSampleCount > 1)
        target = msaaRenderTargetView;
    else
        target = renderTargetView;

    if (target)
        deviceContext->ClearRenderTargetView(target, clearColor);
}

void DirectX11Renderer::ClearDepthStencil(bool clearDepth, bool clearStencil,
                                          float depth, uint8_t stencil) {
    UINT clearFlags = 0;
    if (clearDepth) clearFlags |= D3D11_CLEAR_DEPTH;
    if (clearStencil) clearFlags |= D3D11_CLEAR_STENCIL;

    ID3D11DepthStencilView* target = (msaaSampleCount > 1 && msaaDepthStencilView)
                                      ? msaaDepthStencilView : depthStencilView;
    if (target) {
        deviceContext->ClearDepthStencilView(target, clearFlags, depth, stencil);
    }
}

// Buffer binding
void DirectX11Renderer::BindVertexBuffer(RefPtr<BufferBase> buffer, uint32_t slot) {
    if (!buffer) 
        return;

    UINT stride = sizeof(Sleak::Vertex);
    UINT offset = 0;

    try
    {
        auto d3d11Buffer = dynamic_cast<DirectX11Buffer*>
        (buffer.get())
        ->GetD3DBuffer();

        deviceContext->IASetVertexBuffers(slot, 1, &d3d11Buffer, &stride, &offset);

    }
    catch (std::exception& e)
    {
        SLEAK_ERROR("Failed to cast Vertex Buffer! {}", e.what());
    }
}

void DirectX11Renderer::BindIndexBuffer(RefPtr<BufferBase> buffer,
                             uint32_t slot) {
    if (!buffer) return;

    UINT offset = 0;

    try {
        auto d3d11Buffer =
            dynamic_cast<DirectX11Buffer*>(buffer.get())->GetD3DBuffer();

        deviceContext->IASetIndexBuffer(d3d11Buffer, DXGI_FORMAT_R32_UINT, 0);

    } catch (std::exception& e) {
        SLEAK_ERROR("Failed to cast Index Buffer! {}", e.what());
    }
}

void DirectX11Renderer::BindConstantBuffer(RefPtr<BufferBase> buffer,
                                uint32_t slot) {
    try {
        auto* dx11Buf = dynamic_cast<DirectX11Buffer*>(buffer.get());
        if (!dx11Buf) return;

        // Shadow pass: use dedicated shadow CB with LightVP*World for slot 0
        if (m_inShadowPass && slot == 0 && dx11Buf->GetCPUShadowCopySize() >= 128) {
            const float* srcWorld = reinterpret_cast<const float*>(
                static_cast<const char*>(dx11Buf->GetCPUShadowCopy()) + 64);

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
            std::memcpy(&shadowPC[16], srcWorld, sizeof(float) * 16);

            // Create dedicated shadow transform CB on first use
            if (!m_shadowTransformCB) {
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = 128;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                device->CreateBuffer(&desc, nullptr, &m_shadowTransformCB);
            }

            // Update and bind the shadow CB instead of the original
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(deviceContext->Map(m_shadowTransformCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, shadowPC, sizeof(shadowPC));
                deviceContext->Unmap(m_shadowTransformCB, 0);
            }
            deviceContext->VSSetConstantBuffers(slot, 1, &m_shadowTransformCB);
            deviceContext->PSSetConstantBuffers(slot, 1, &m_shadowTransformCB);
            return;
        }

        auto d3d11Buffer = dx11Buf->GetD3DBuffer();
        deviceContext->VSSetConstantBuffers(slot, 1, &d3d11Buffer);
        deviceContext->PSSetConstantBuffers(slot, 1, &d3d11Buffer);

    } catch (std::exception& e) {
        SLEAK_ERROR("Failed to cast Constant Buffer! {}", e.what());
    }
}

void DirectX11Renderer::ConfigureRenderMode() {
    switch (Mode)
    {
    case RenderMode::Fill:
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        fillMode = D3D11_FILL_MODE::D3D11_FILL_SOLID;
        break;
    case RenderMode::Points: 
        topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        fillMode = D3D11_FILL_MODE::D3D11_FILL_SOLID;
        break;
    case RenderMode::Wireframe : 
        topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        fillMode = D3D11_FILL_MODE::D3D11_FILL_WIREFRAME;
        break;
    default:
        SLEAK_ERROR("Unable to set render mode!");
        break;
    }

    if (deviceContext)
        deviceContext->IASetPrimitiveTopology(topology);

    SetRasterState();
}

void DirectX11Renderer::ConfigureRenderFace() {
    switch(Face) {
        case RenderFace::None : cull = D3D11_CULL_MODE::D3D11_CULL_NONE; break;
        case RenderFace::Front : cull = D3D11_CULL_MODE::D3D11_CULL_FRONT; break;
        case RenderFace::Back : cull = D3D11_CULL_MODE::D3D11_CULL_BACK; break;
        default: cull = D3D11_CULL_MODE::D3D11_CULL_FRONT;
    }   
}

bool DirectX11Renderer::SetRasterState() {
    ID3D11RasterizerState* rasterState;
    D3D11_RASTERIZER_DESC RasterDesc = {};
    RasterDesc.AntialiasedLineEnable = true;
    RasterDesc.MultisampleEnable = true;
    RasterDesc.CullMode = cull;
    RasterDesc.DepthBias = 0; 
    RasterDesc.DepthBiasClamp = 0.0f;
    RasterDesc.SlopeScaledDepthBias = 0.0f;
    RasterDesc.DepthClipEnable = true;
    RasterDesc.ScissorEnable = false;
    RasterDesc.FillMode = fillMode;
    RasterDesc.FrontCounterClockwise = false;

    if (FAILED(device->CreateRasterizerState(&RasterDesc, &rasterState))) {
        return false;
    } else {
        deviceContext->RSSetState(rasterState);
        rasterState->Release();
    }

    return true;
}

void DirectX11Renderer::SetRenderMode(RenderMode mode) {
    this->Mode = mode;
    ConfigureRenderMode();
}

void DirectX11Renderer::SetRenderFace(RenderFace face) {
    this->Face = face;
    ConfigureRenderFace();
}

void DirectX11Renderer::SetViewport(float x, float y, float width, float height,
                                    float minDepth,
                                    float maxDepth) {
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = minDepth;
    viewport.MaxDepth = maxDepth;
    viewport.TopLeftX = x;
    viewport.TopLeftY = y;
    deviceContext->RSSetViewports(1, &viewport);

}

bool DirectX11Renderer::CreateRenderTargetView() {
    // Release the existing render target view if it exists
    if (renderTargetView) {
        renderTargetView->Release();
        renderTargetView = nullptr;
    }

    // Get the back buffer from the swap chain
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                      reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to get back buffer!");
        return false;
    }

    // Create the render target view
    hr = device->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView);
    backBuffer->Release();
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create render target view!");
        return false;
    }

    // Set the render target
    deviceContext->OMSetRenderTargets(1, &renderTargetView, nullptr);

    SLEAK_INFO("Render target view created successfully.");
    return true;
}

Texture* DirectX11Renderer::CreateTextureFromData(uint32_t width, uint32_t height, void* data) {
    DirectX11Texture* texture = new DirectX11Texture(device);
    if (texture->LoadFromMemory(data,width,height,TextureFormat::RGBA8)) {
        return texture;
    }
    return nullptr;
}

Texture* DirectX11Renderer::CreateTexture(const std::string& TexturePath) {
    DirectX11Texture* texture = new DirectX11Texture(device);
    if (texture->LoadFromFile(TexturePath)) {
        return texture;
    }
    return nullptr;
}

Texture* DirectX11Renderer::CreateCubemapTexture(
    const std::array<std::string, 6>& facePaths) {
    auto* texture = new DirectX11CubemapTexture(device);
    if (texture->LoadCubemap(facePaths)) {
        return texture;
    }
    delete texture;
    return nullptr;
}

Texture* DirectX11Renderer::CreateCubemapTextureFromPanorama(
    const std::string& panoramaPath) {
    auto* texture = new DirectX11CubemapTexture(device);
    if (texture->LoadEquirectangular(panoramaPath)) {
        return texture;
    }
    delete texture;
    return nullptr;
}

void DirectX11Renderer::SetDepthWrite(bool enabled) {
    m_depthWriteEnabled = enabled;

    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = enabled ? D3D11_DEPTH_WRITE_MASK_ALL
                                  : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = m_depthFunc;
    desc.StencilEnable = FALSE;

    ID3D11DepthStencilState* state = nullptr;
    if (SUCCEEDED(device->CreateDepthStencilState(&desc, &state))) {
        deviceContext->OMSetDepthStencilState(state, 1);
        state->Release();
    }
}

void DirectX11Renderer::SetDepthCompare(DepthCompare compare) {
    switch (compare) {
        case DepthCompare::Less:         m_depthFunc = D3D11_COMPARISON_LESS; break;
        case DepthCompare::LessEqual:    m_depthFunc = D3D11_COMPARISON_LESS_EQUAL; break;
        case DepthCompare::Greater:      m_depthFunc = D3D11_COMPARISON_GREATER; break;
        case DepthCompare::GreaterEqual: m_depthFunc = D3D11_COMPARISON_GREATER_EQUAL; break;
        case DepthCompare::Equal:        m_depthFunc = D3D11_COMPARISON_EQUAL; break;
        case DepthCompare::NotEqual:     m_depthFunc = D3D11_COMPARISON_NOT_EQUAL; break;
        case DepthCompare::Always:       m_depthFunc = D3D11_COMPARISON_ALWAYS; break;
        case DepthCompare::Never:        m_depthFunc = D3D11_COMPARISON_NEVER; break;
    }

    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = m_depthWriteEnabled ? D3D11_DEPTH_WRITE_MASK_ALL
                                               : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = m_depthFunc;
    desc.StencilEnable = FALSE;

    ID3D11DepthStencilState* state = nullptr;
    if (SUCCEEDED(device->CreateDepthStencilState(&desc, &state))) {
        deviceContext->OMSetDepthStencilState(state, 1);
        state->Release();
    }
}

void DirectX11Renderer::SetCullEnabled(bool enabled) {
    if (enabled) {
        cull = D3D11_CULL_FRONT;
    } else {
        cull = D3D11_CULL_NONE;
    }
    SetRasterState();
}

void DirectX11Renderer::BindTexture(RefPtr<Sleak::Texture> texture,
                                     uint32_t slot) {
    if (texture.IsValid()) {
        texture->Bind(slot);
    }
}

void DirectX11Renderer::BeginSkyboxPass() {
    // Save current state
    m_savedCullMode = cull;
    m_savedDepthStencilState = depthStencilState;
    if (m_savedDepthStencilState) {
        m_savedDepthStencilState->AddRef();
    }
}

void DirectX11Renderer::EndSkyboxPass() {
    // Restore depth stencil state
    if (m_savedDepthStencilState) {
        deviceContext->OMSetDepthStencilState(m_savedDepthStencilState, 1);
        m_savedDepthStencilState->Release();
        m_savedDepthStencilState = nullptr;
    }

    // Restore cull mode
    cull = m_savedCullMode;
    SetRasterState();

    // Reset depth tracking
    m_depthWriteEnabled = true;
    m_depthFunc = D3D11_COMPARISON_LESS;
}

void DirectX11Renderer::BeginDebugLinePass() {
    m_savedTopology = topology;
    topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
}

void DirectX11Renderer::EndDebugLinePass() {
    topology = m_savedTopology;
    deviceContext->IASetPrimitiveTopology(topology);
}

BufferBase* DirectX11Renderer::CreateBuffer(BufferType Type, uint32_t size, void* data) {
    assert(size > 0);

    DirectX11Buffer* buffer = new DirectX11Buffer(device,size, Type);
    buffer->Initialize(data);
    return static_cast<BufferBase*>(buffer);
}

Shader* DirectX11Renderer::CreateShader(const std::string& shaderSource) {
    DirectX11Shader* shader = new DirectX11Shader(device);
    if (shader->compile(shaderSource)) {
        // Each shader creates its own input layout matching its VS inputs.
        // The layout is stored per-shader and set in bind().
        auto* il = shader->createInputLayout();
        if (!bIsLayoutCreated && il) {
            layout = il;
            bIsLayoutCreated = true;
        }
        return static_cast<Shader*>(shader);
    }
    return nullptr;
}

bool DirectX11Renderer::CreateBlendState() {
    //(+) : Binary Operator
    // Color = SourceP (X) SourceFactor (+) DestP (X) DestFactor
    // Alpha = SourceA SourceFactor (+) DestA DestFactor

    D3D11_BLEND_DESC desc;
    desc.AlphaToCoverageEnable = false;
    desc.IndependentBlendEnable = false;

    desc.RenderTarget[0].BlendEnable = true;

    desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    
    desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    
    desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(device->CreateBlendState(&desc, &blendState))) 
        return false;

    return true;
}

void DirectX11Renderer::SetBlendState(float r, float g, float b, float a, UINT Mask) {
    float BlendFactor[4] = {r,g,b,a};
    deviceContext->OMSetBlendState(blendState, BlendFactor, Mask);
}

void DirectX11Renderer::CheckMSAASupport() {
    m_maxMsaaSampleCount = 1;
    for (UINT sampleCount = 1;
         sampleCount <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT;
         sampleCount *= 2)
    {
        UINT quality = 0;
        HRESULT hr = device->CheckMultisampleQualityLevels(
            DXGI_FORMAT_R8G8B8A8_UNORM, sampleCount, &quality);
        if (SUCCEEDED(hr) && quality > 0) {
            SLEAK_INFO("MSAA {}x supported with {} quality levels.",
                       sampleCount, quality);
            if (sampleCount <= 8)
                m_maxMsaaSampleCount = sampleCount;
        }
    }
}

bool DirectX11Renderer::CreateMSAARenderTarget() { 

    if (msaaRenderTarget) msaaRenderTarget->Release();
    if (msaaRenderTargetView) msaaRenderTargetView->Release();

    D3D11_TEXTURE2D_DESC texDesc;
    texDesc.Width = window->GetWidth();
    texDesc.Height = window->GetHeight();
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.SampleDesc.Count = msaaSampleCount;
    texDesc.SampleDesc.Quality = msaaQualityLevel;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    if (FAILED(device->CreateTexture2D(&texDesc, nullptr, &msaaRenderTarget))) 
        return false;

    if (FAILED(device->CreateRenderTargetView(msaaRenderTarget,nullptr,&msaaRenderTargetView))) 
        return false;

    return true; 
}

bool DirectX11Renderer::CreateMSAADepthStencil() {
    if (msaaDepthStencilBuffer) msaaDepthStencilBuffer->Release();
    if (msaaDepthStencilView) msaaDepthStencilView->Release();

    D3D11_TEXTURE2D_DESC texDesc;
    texDesc.Width = window->GetWidth();
    texDesc.Height = window->GetHeight();
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.SampleDesc.Count = msaaSampleCount;
    texDesc.SampleDesc.Quality = msaaQualityLevel;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

    if (FAILED(device->CreateTexture2D(&texDesc,nullptr,&msaaDepthStencilBuffer))) return false;

    if (FAILED(device->CreateDepthStencilView(msaaDepthStencilBuffer,nullptr,&msaaDepthStencilView))) return false;


    return true;
}

void DirectX11Renderer::ResolveMSAA() {
    if (msaaSampleCount <= 1 || msaaSampleCount % 2 != 0) 
        return;

    ID3D11Texture2D* back;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) {
        SLEAK_ERROR("Failed to get back buffer for MSAA!");
        return;
    }
    deviceContext->ResolveSubresource(back,0,msaaRenderTarget,0,DXGI_FORMAT_R8G8B8A8_UNORM);

    back->Release();
}

void DirectX11Renderer::ApplyMSAAChange() {
    if (!m_msaaChangeRequested)
        return;
    m_msaaChangeRequested = false;

    m_msaaSampleCount = m_pendingMsaaSampleCount;
    msaaSampleCount = m_pendingMsaaSampleCount;

    // Query quality level for new sample count
    UINT quality = 0;
    device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM,
                                           msaaSampleCount, &quality);
    msaaQualityLevel = (quality > 0) ? quality - 1 : 0;

    // Cleanup old MSAA resources
    if (msaaRenderTarget) { msaaRenderTarget->Release(); msaaRenderTarget = nullptr; }
    if (msaaRenderTargetView) { msaaRenderTargetView->Release(); msaaRenderTargetView = nullptr; }
    if (msaaDepthStencilBuffer) { msaaDepthStencilBuffer->Release(); msaaDepthStencilBuffer = nullptr; }
    if (msaaDepthStencilView) { msaaDepthStencilView->Release(); msaaDepthStencilView = nullptr; }

    if (msaaSampleCount > 1) {
        CreateMSAARenderTarget();
        CreateMSAADepthStencil();
    } else {
        // Re-bind non-MSAA render targets since pipeline still references released MSAA views
        deviceContext->OMSetRenderTargets(1, &renderTargetView, depthStencilView);
    }

    SLEAK_INFO("DX11 MSAA changed to {}x", msaaSampleCount);
}

bool DirectX11Renderer::CreateImGUI()
{
    IMGUI_CHECKVERSION();
    ImCon = ImGui::CreateContext();
    ImCon->IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if(!ImGui_ImplSDL3_InitForD3D(window->GetSDLWindow()))
        return false;

    if(!ImGui_ImplDX11_Init(device, deviceContext))
        return false;

    if(!ImGui_ImplDX11_CreateDeviceObjects())
        return false;


    bImInitialized = true;

    return true;
}


// ---- Shadow Constant Buffer ----

bool DirectX11Renderer::CreateShadowConstantBuffer() {
    if (m_shadowCBCreated) return true;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(PCSSShadowGPUData);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device->CreateBuffer(&desc, nullptr, &m_shadowCB);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create shadow constant buffer!");
        return false;
    }

    m_shadowCBCreated = true;
    return true;
}

void DirectX11Renderer::CleanupShadowResources() {
    if (m_shadowCB) { m_shadowCB->Release(); m_shadowCB = nullptr; }
    m_shadowCBCreated = false;
    if (m_shadowTransformCB) { m_shadowTransformCB->Release(); m_shadowTransformCB = nullptr; }
    if (m_shadowRasterState) { m_shadowRasterState->Release(); m_shadowRasterState = nullptr; }
    if (m_shadowSampler) { m_shadowSampler->Release(); m_shadowSampler = nullptr; }
    if (m_shadowSRV) { m_shadowSRV->Release(); m_shadowSRV = nullptr; }
    if (m_shadowDSV) { m_shadowDSV->Release(); m_shadowDSV = nullptr; }
    if (m_shadowDepthTex) { m_shadowDepthTex->Release(); m_shadowDepthTex = nullptr; }
    m_shadowMapCreated = false;
}

void DirectX11Renderer::SetLightVP(const float* mat) {
    if (mat) std::memcpy(m_lightVP, mat, sizeof(m_lightVP));
}

bool DirectX11Renderer::CreateShadowMapResources() {
    if (m_shadowMapCreated) return true;

    // Create depth texture
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = SHADOW_MAP_SIZE;
    texDesc.Height = SHADOW_MAP_SIZE;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &m_shadowDepthTex);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create shadow depth texture!");
        return false;
    }

    // Create DSV
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = device->CreateDepthStencilView(m_shadowDepthTex, &dsvDesc, &m_shadowDSV);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create shadow DSV!");
        return false;
    }

    // Create SRV for sampling
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(m_shadowDepthTex, &srvDesc, &m_shadowSRV);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create shadow SRV!");
        return false;
    }

    // Create comparison sampler
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sampDesc.BorderColor[0] = 1.0f;
    sampDesc.BorderColor[1] = 1.0f;
    sampDesc.BorderColor[2] = 1.0f;
    sampDesc.BorderColor[3] = 1.0f;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = 0;
    hr = device->CreateSamplerState(&sampDesc, &m_shadowSampler);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create shadow sampler!");
        return false;
    }

    // Create shadow rasterizer state with depth bias
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE; // all faces cast shadow regardless of orientation
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = 100;
    rasterDesc.SlopeScaledDepthBias = 2.0f;
    rasterDesc.DepthBiasClamp = 0.01f;
    rasterDesc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rasterDesc, &m_shadowRasterState);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create shadow rasterizer state!");
        return false;
    }

    m_shadowMapCreated = true;
    SLEAK_INFO("D3D11 shadow map resources created ({}x{})", SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    return true;
}

void DirectX11Renderer::RenderShadowPass() {
    // Skip if no cached draws — preserve previous frame's shadow map
    auto* queue = RenderCommandQueue::GetInstance();
    if (!queue || !queue->HasCachedShadowDraws()) return;

    if (!m_shadowMapCreated) {
        if (!CreateShadowMapResources()) return;
    }

    // Save current render targets and viewport
    ID3D11RenderTargetView* savedRTV = nullptr;
    ID3D11DepthStencilView* savedDSV = nullptr;
    deviceContext->OMGetRenderTargets(1, &savedRTV, &savedDSV);

    D3D11_VIEWPORT savedViewport;
    UINT numViewports = 1;
    deviceContext->RSGetViewports(&numViewports, &savedViewport);

    // Unbind shadow SRV to avoid D3D11 warning (resource bound as both input and output)
    ID3D11ShaderResourceView* nullSRV = nullptr;
    deviceContext->PSSetShaderResources(3, 1, &nullSRV);

    // Set shadow render target (depth only, no color)
    ID3D11RenderTargetView* nullRTV = nullptr;
    deviceContext->OMSetRenderTargets(1, &nullRTV, m_shadowDSV);

    // Set shadow viewport
    D3D11_VIEWPORT shadowViewport{};
    shadowViewport.Width = static_cast<float>(SHADOW_MAP_SIZE);
    shadowViewport.Height = static_cast<float>(SHADOW_MAP_SIZE);
    shadowViewport.MinDepth = 0.0f;
    shadowViewport.MaxDepth = 1.0f;
    deviceContext->RSSetViewports(1, &shadowViewport);

    // Clear shadow depth
    deviceContext->ClearDepthStencilView(m_shadowDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Set shadow rasterizer state
    deviceContext->RSSetState(m_shadowRasterState);

    // Set primitive topology (same as main pass)
    deviceContext->IASetPrimitiveTopology(topology);
    if (bIsLayoutCreated)
        deviceContext->IASetInputLayout(layout);

    // Execute shadow draw commands
    m_inShadowPass = true;
    if (queue) {
        queue->ExecuteShadowPass(this);
    }
    m_inShadowPass = false;

    // Restore raster state
    SetRasterState();

    // Restore render targets and viewport
    deviceContext->OMSetRenderTargets(1, &savedRTV, savedDSV);
    deviceContext->RSSetViewports(1, &savedViewport);
    if (savedRTV) savedRTV->Release();
    if (savedDSV) savedDSV->Release();

    // Bind shadow map SRV and sampler for main pass
    deviceContext->PSSetShaderResources(3, 1, &m_shadowSRV);
    deviceContext->PSSetSamplers(3, 1, &m_shadowSampler);
}

void DirectX11Renderer::UpdateShadowLightUBO(const void* data, uint32_t size) {
    if (!device || !deviceContext) return;

    // Create shadow CB on first call
    if (!m_shadowCBCreated) {
        if (!CreateShadowConstantBuffer()) return;
    }

    // Map the ShadowLightUBO data into our PCSSShadowGPUData format
    const auto* ubo = static_cast<const ShadowLightUBO*>(data);

    PCSSShadowGPUData shadowData{};
    std::memcpy(shadowData.LightVP, ubo->LightVP, sizeof(float) * 16);
    shadowData.ShadowBias = ubo->ShadowBias;
    shadowData.ShadowStrength = ubo->ShadowStrength;
    shadowData.ShadowTexelSize = ubo->ShadowTexelSize;
    shadowData.ShadowLightSize = ubo->LightSize;
    shadowData.PCSSEnabled = m_pcssEnabled ? 1 : 0;
    shadowData.ShadowMapEnabled = m_shadowPassEnabled ? 1 : 0;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = deviceContext->Map(m_shadowCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        std::memcpy(mapped.pData, &shadowData, sizeof(shadowData));
        deviceContext->Unmap(m_shadowCB, 0);
    }

    // Bind shadow CB at slot 5 (both VS and PS)
    deviceContext->VSSetConstantBuffers(5, 1, &m_shadowCB);
    deviceContext->PSSetConstantBuffers(5, 1, &m_shadowCB);
}

// ---- Post-Process: Tonemapping ----

bool DirectX11Renderer::CreateTonemapResources() {
    if (m_tonemapResourcesCreated) return true;
    if (!device) return false;

    // Get back buffer dimensions
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (!backBuffer) return false;

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);
    backBuffer->Release();

    // Create HDR render target (R16G16B16A16_FLOAT)
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = bbDesc.Width;
    texDesc.Height = bbDesc.Height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &m_hdrTexture);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create HDR texture for tonemapping!");
        return false;
    }

    hr = device->CreateRenderTargetView(m_hdrTexture, nullptr, &m_hdrRTV);
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(m_hdrTexture, nullptr, &m_hdrSRV);
    if (FAILED(hr)) return false;

    // Create point sampler
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = device->CreateSamplerState(&sampDesc, &m_pointSampler);
    if (FAILED(hr)) return false;

    // Create post-process constant buffer
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(PostProcessGPUData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, nullptr, &m_postProcessCB);
    if (FAILED(hr)) return false;

    // Compile tonemap shaders
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    // Build path to shader (relative to working directory = bin/)
    std::string narrowPath = "assets/shaders/tonemap.hlsl";
    std::wstring shaderPath(narrowPath.begin(), narrowPath.end());

    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VS_Main", "vs_5_0", D3DCOMPILE_DEBUG, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            SLEAK_ERROR("Tonemap VS compile error: {}",
                (const char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return false;
    }

    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PS_Main", "ps_5_0", D3DCOMPILE_DEBUG, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            SLEAK_ERROR("Tonemap PS compile error: {}",
                (const char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        vsBlob->Release();
        return false;
    }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(), nullptr, &m_tonemapVS);
    vsBlob->Release();
    if (FAILED(hr)) { psBlob->Release(); return false; }

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(),
        psBlob->GetBufferSize(), nullptr, &m_tonemapPS);
    psBlob->Release();
    if (FAILED(hr)) return false;

    m_tonemapResourcesCreated = true;
    SLEAK_INFO("Tonemapping post-process resources created successfully");
    return true;
}

void DirectX11Renderer::CleanupTonemapResources() {
    if (m_tonemapVS) { m_tonemapVS->Release(); m_tonemapVS = nullptr; }
    if (m_tonemapPS) { m_tonemapPS->Release(); m_tonemapPS = nullptr; }
    if (m_postProcessCB) { m_postProcessCB->Release(); m_postProcessCB = nullptr; }
    if (m_hdrSRV) { m_hdrSRV->Release(); m_hdrSRV = nullptr; }
    if (m_hdrRTV) { m_hdrRTV->Release(); m_hdrRTV = nullptr; }
    if (m_hdrTexture) { m_hdrTexture->Release(); m_hdrTexture = nullptr; }
    if (m_pointSampler) { m_pointSampler->Release(); m_pointSampler = nullptr; }
    m_tonemapResourcesCreated = false;
}

void DirectX11Renderer::ExecuteTonemapPass() {
    if (!m_tonemapResourcesCreated || !m_tonemapVS || !m_tonemapPS)
        return;

    // Update post-process constant buffer
    PostProcessGPUData ppData{};
    ppData.Exposure = m_exposure;
    ppData.Gamma = m_gamma;
    ppData.TonemapEnabled = m_tonemapEnabled ? 1 : 0;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = deviceContext->Map(m_postProcessCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        std::memcpy(mapped.pData, &ppData, sizeof(ppData));
        deviceContext->Unmap(m_postProcessCB, 0);
    }

    // Switch render target to backbuffer
    deviceContext->OMSetRenderTargets(1, &renderTargetView, nullptr);

    // Bind HDR texture as input
    deviceContext->PSSetShaderResources(0, 1, &m_hdrSRV);
    deviceContext->PSSetSamplers(0, 1, &m_pointSampler);
    deviceContext->PSSetConstantBuffers(0, 1, &m_postProcessCB);

    // Set tonemap shaders
    deviceContext->VSSetShader(m_tonemapVS, nullptr, 0);
    deviceContext->PSSetShader(m_tonemapPS, nullptr, 0);

    // Draw fullscreen triangle (no input layout needed for SV_VertexID)
    deviceContext->IASetInputLayout(nullptr);
    deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deviceContext->Draw(3, 0);

    // Unbind HDR SRV to prevent read/write conflict
    ID3D11ShaderResourceView* nullSRV = nullptr;
    deviceContext->PSSetShaderResources(0, 1, &nullSRV);
}

} // namespace RenderEngine
} // namespace Sleak

#else
namespace Sleak::RenderEngine {class DirectX11Renderer;}

#endif