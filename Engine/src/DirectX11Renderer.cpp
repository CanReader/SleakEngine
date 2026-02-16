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
    swapChainDesc.BufferCount = 1;
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
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, true);

            D3D11_MESSAGE_SEVERITY severities[] = {
                D3D11_MESSAGE_SEVERITY_INFO
            };
    
            // Enable all messages
            D3D11_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumSeverities = _countof(severities);
            filter.DenyList.pSeverityList = severities;
    
            infoQueue->AddStorageFilterEntries(&filter);
            infoQueue->Release();
        }

        SetPerformanceCounter(true);
#endif

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
    //SetBlendState(0.5,0.5,0.5,1.0,0xFFFFFF);
    ClearRenderTarget(0.39f, 0.58f, 0.93f, 1.0f);
    ClearDepthStencil(true, false, 1.0, 0);

    if (bIsLayoutCreated)
        deviceContext->IASetInputLayout(layout);

    if(bImInitialized) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
    }
}

void DirectX11Renderer::EndRender() {
    if (bImInitialized) {
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    swapChain->Present(1, 0);

    if(bEnabledPerformanceCounter) {
        auto now = std::chrono::high_resolution_clock::now();
        FrameTimes.push(FrameTimer.Elapsed() * 1000);
        FrameTimer.Reset();

        if(FrameTimes.size() > FrameTimeHistory)
        FrameTimes.pop();
    
        float total = 0.0f;
        for(float val : FrameTimes)
            total += val;
    
        frameTime = total / FrameTimes.size();
        frameRate = frameTime  > 0 ? static_cast<int>(1000 / frameTime) : 0;
     
        DrawnVertices = 0;
        DrawnTriangles = 0;
    }
}

void DirectX11Renderer::Cleanup() {
    bImInitialized = false;

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
        ID3D11Debug* debugDevice;
        device->QueryInterface(__uuidof(ID3D11Debug), (void**)&debugDevice);
        if (debugDevice) {
            debugDevice->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
            debugDevice->Release();
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

    // Resize the swap chain buffers
    HRESULT hr =
        swapChain->ResizeBuffers(1,                           // Buffer count
                                 width,                       // New width
                                 height,                      // New height
                                 DXGI_FORMAT_R8G8B8A8_UNORM,  // Buffer format
                                 0                            // Flags
        );

    SetViewport(0,0,width,height);

    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to resize swap chain buffers!");
        return;
    }

    // Recreate the render target view
    if (!CreateRenderTargetView() || !CreateDepthStencilBuffer(width, height)) {
        return;
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

    ID3D11RenderTargetView* target = msaaSampleCount > 1 ? msaaRenderTargetView : renderTargetView; 

    deviceContext->ClearRenderTargetView(target, clearColor);
}

void DirectX11Renderer::ClearDepthStencil(bool clearDepth, bool clearStencil,
                                          float depth, uint8_t stencil) {
    UINT clearFlags = 0;
    if (clearDepth) clearFlags |= D3D11_CLEAR_DEPTH;
    if (clearStencil) clearFlags |= D3D11_CLEAR_STENCIL;

    if (depthStencilView) {
        deviceContext->ClearDepthStencilView(depthStencilView, clearFlags,depth, stencil);
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
        auto d3d11Buffer =
            dynamic_cast<DirectX11Buffer*>(buffer.get())->GetD3DBuffer();

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

BufferBase* DirectX11Renderer::CreateBuffer(BufferType Type, uint32_t size, void* data) {
    assert(size > 0);

    DirectX11Buffer* buffer = new DirectX11Buffer(device,size, Type);
    buffer->Initialize(data);
    return static_cast<BufferBase*>(buffer);
}

Shader* DirectX11Renderer::CreateShader(const std::string& shaderSource) {
    DirectX11Shader* shader = new DirectX11Shader(device);
    if (shader->compile(shaderSource)) {
        layout = shader->createInputLayout();
        bIsLayoutCreated = true;
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
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)back))) {
        SLEAK_ERROR("Failed to get back buffer for MSAA!");
        return;
    }
    deviceContext->ResolveSubresource(back,0,msaaRenderTarget,0,DXGI_FORMAT_R8G8B8A8_UNORM);

    back->Release();
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


} // namespace RenderEngine
} // namespace Sleak

#else
namespace Sleak::RenderEngine {class DirectX11Renderer;} 

#endif