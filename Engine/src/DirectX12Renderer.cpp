#include "../../include/private/Graphics/DirectX/DirectX12Renderer.hpp"
#ifdef PLATFORM_WIN

#include <Window.hpp>
#include <SDL3/SDL_system.h>
#include <Graphics/Vertex.hpp>
#include <Graphics/DirectX/DirectX12CubemapTexture.hpp>
#include <Logger.hpp>
#include <stdexcept>
#include <string>
#include <locale>
#include <codecvt>
#include <d3dcompiler.h>

namespace Sleak {
namespace RenderEngine {

// -----------------------------------------------------------------------
// Constructor / Destructor
// -----------------------------------------------------------------------

DirectX12Renderer::DirectX12Renderer(Window* window) : window(window) {
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent == nullptr) {
        throw std::runtime_error("Failed to create fence event.");
    }
    this->Type = RendererType::DirectX12;

    ResourceManager::RegisterCreateBuffer(
        this, &DirectX12Renderer::CreateBuffer);
    ResourceManager::RegisterCreateShader(
        this, &DirectX12Renderer::CreateShader);
    ResourceManager::RegisterCreateTexture(
        this, &DirectX12Renderer::CreateTexture);
    ResourceManager::RegisterCreateCubemapTexture(
        this, &DirectX12Renderer::CreateCubemapTexture);
    ResourceManager::RegisterCreateCubemapTextureFromPanorama(
        this, &DirectX12Renderer::CreateCubemapTextureFromPanorama);
    ResourceManager::RegisterCreateTextureFromMemory(
        [this](const void* data, uint32_t w, uint32_t h, TextureFormat fmt) -> Texture* {
            auto* tex = new DirectX12Texture(device.Get(), commandQueue.Get(), commandList.Get());
            if (tex->LoadFromMemory(data, w, h, fmt)) return tex;
            delete tex;
            return nullptr;
        });
}

DirectX12Renderer::~DirectX12Renderer() {
    WaitForGPU();
    delete m_defaultTexture;
    m_defaultTexture = nullptr;
    Cleanup();
    if (fenceEvent) {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
}

// -----------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------

bool DirectX12Renderer::Initialize() {
    if (m_Initialized) return true;

    if (!CreateDevice()) return false;
    if (!CreateCommandQueue()) return false;
    if (!CreateCommandAllocatorAndList()) return false;
    if (!CreateSwapChain()) return false;
    if (!CreateRenderTargetViews()) return false;
    if (!CreateDepthStencilView()) return false;
    if (!CreateFence()) return false;
    if (!CreateRootSignature()) return false;
    // PSO is created lazily when CreateShader() is called

    // Create a 1x1 white default texture so the SRV table is always valid
    {
        uint32_t whitePixel = 0xFFFFFFFF;  // RGBA(255,255,255,255)
        m_defaultTexture = new DirectX12Texture(
            device.Get(), commandQueue.Get(), commandList.Get());
        if (!m_defaultTexture->LoadFromMemory(
                &whitePixel, 1, 1, TextureFormat::RGBA8)) {
            SLEAK_WARN("Failed to create default white texture for DX12");
            delete m_defaultTexture;
            m_defaultTexture = nullptr;
        }
    }

    // Set initial viewport
    int w = Window::GetWidth();
    int h = Window::GetHeight();
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(w);
    viewport.Height = static_cast<float>(h);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    scissorRect.left = 0;
    scissorRect.top = 0;
    scissorRect.right = w;
    scissorRect.bottom = h;

    m_Initialized = true;
    SLEAK_INFO("DirectX 12 has been initialized successfully!");

    return true;
}

bool DirectX12Renderer::CreateDevice() {
    // Try feature level 12.0 first
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                                     IID_PPV_ARGS(&device)))) {
        SLEAK_INFO("DirectX 12 device created with feature level 12.0");
        return true;
    }

    // Fall back to 11.0
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                     IID_PPV_ARGS(&device)))) {
        SLEAK_INFO("DirectX 12 device created with feature level 11.0");
        return true;
    }

    SLEAK_ERROR("Failed to create DirectX 12 device!");
    return false;
}

bool DirectX12Renderer::CreateCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    if (FAILED(device->CreateCommandQueue(&queueDesc,
                                           IID_PPV_ARGS(&commandQueue)))) {
        SLEAK_ERROR("Failed to create command queue!");
        return false;
    }
    return true;
}

bool DirectX12Renderer::CreateCommandAllocatorAndList() {
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&commandAllocator)))) {
        SLEAK_ERROR("Failed to create command allocator!");
        return false;
    }

    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(),
            nullptr, IID_PPV_ARGS(&commandList)))) {
        SLEAK_ERROR("Failed to create command list!");
        return false;
    }

    // Close the command list (it starts in an open state)
    commandList->Close();
    return true;
}

bool DirectX12Renderer::CreateSwapChain() {
    HWND hwnd = (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window->GetSDLWindow()),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = 0;
    swapChainDesc.Height = 0;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        SLEAK_ERROR("Failed to create DXGI factory!");
        return false;
    }

    HRESULT hr = factory->CreateSwapChainForHwnd(
        commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr,
        reinterpret_cast<IDXGISwapChain1**>(swapChain.GetAddressOf()));

    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create swap chain!");
        return false;
    }

    frameIndex = swapChain->GetCurrentBackBufferIndex();

    EnumerateDevices(factory);

    return true;
}

bool DirectX12Renderer::CreateRenderTargetViews() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc,
                                             IID_PPV_ARGS(&rtvHeap)))) {
        SLEAK_ERROR("Failed to create RTV descriptor heap!");
        return false;
    }

    rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < FrameCount; i++) {
        if (FAILED(swapChain->GetBuffer(i,
                                         IID_PPV_ARGS(&renderTargets[i])))) {
            SLEAK_ERROR("Failed to get swap chain buffer {}!", i);
            return false;
        }
        device->CreateRenderTargetView(renderTargets[i].Get(), nullptr,
                                       rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize;
    }

    return true;
}

bool DirectX12Renderer::CreateDepthStencilView() {
    // Create DSV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(device->CreateDescriptorHeap(&dsvHeapDesc,
                                             IID_PPV_ARGS(&dsvHeap)))) {
        SLEAK_ERROR("Failed to create DSV descriptor heap!");
        return false;
    }

    // Get swap chain dimensions
    DXGI_SWAP_CHAIN_DESC1 scDesc;
    swapChain->GetDesc1(&scDesc);
    UINT width = scDesc.Width;
    UINT height = scDesc.Height;

    // Create depth stencil buffer
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
            IID_PPV_ARGS(&depthStencilBuffer)))) {
        SLEAK_ERROR("Failed to create depth stencil buffer!");
        return false;
    }

    // Create depth stencil view
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

    device->CreateDepthStencilView(
        depthStencilBuffer.Get(), &dsvDesc,
        dsvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

bool DirectX12Renderer::CreateFence() {
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&fence)))) {
        SLEAK_ERROR("Failed to create fence!");
        return false;
    }
    fenceValue = 1;
    return true;
}

bool DirectX12Renderer::CreateRootSignature() {
    // Parameter 0: CBV at register(b0), vertex shader
    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // Parameter 1: SRV descriptor table at register(t0), pixel shader
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static sampler at register(s0), pixel shader
    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MipLODBias = 0.0f;
    staticSampler.MaxAnisotropy = 1;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.BorderColor =
        D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSampler.MinLOD = 0.0f;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0;
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &staticSampler;
    rootSigDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3D12SerializeRootSignature(
        &rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRootSig, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            SLEAK_ERROR("Root signature serialization error: {}",
                        (char*)errorBlob->GetBufferPointer());
        }
        return false;
    }

    hr = device->CreateRootSignature(
        0, serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&rootSignature));

    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create root signature!");
        return false;
    }

    return true;
}

bool DirectX12Renderer::CreatePipelineState() {
    // Called from CreateShader when we have compiled shader blobs.
    // Requires vertexShaderBlob and pixelShaderBlob to be set on
    // the DirectX12Shader before calling this.
    // This is a no-op placeholder — PSO creation happens in CreateShader.
    return true;
}

bool DirectX12Renderer::CreatePipelineStateFromShader(
    ID3DBlob* vertexShaderBlob, ID3DBlob* pixelShaderBlob) {
    if (!vertexShaderBlob || !pixelShaderBlob) return false;

    // Input layout matching Sleak::Vertex
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, px)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, nx)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, tx)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, r)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, u)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = {vertexShaderBlob->GetBufferPointer(),
                  vertexShaderBlob->GetBufferSize()};
    psoDesc.PS = {pixelShaderBlob->GetBufferPointer(),
                  pixelShaderBlob->GetBufferSize()};

    // Rasterizer state
    D3D12_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = D3D12_CULL_MODE_FRONT;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = 0;
    rasterDesc.DepthBiasClamp = 0.0f;
    rasterDesc.SlopeScaledDepthBias = 0.0f;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;
    rasterDesc.ForcedSampleCount = 0;
    rasterDesc.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    psoDesc.RasterizerState = rasterDesc;

    // Blend state
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC rtBlendDesc = {};
    rtBlendDesc.BlendEnable = TRUE;
    rtBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rtBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rtBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        blendDesc.RenderTarget[i] = rtBlendDesc;
    psoDesc.BlendState = blendDesc;

    // Depth stencil state
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;
    psoDesc.DepthStencilState = depthStencilDesc;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    HRESULT hr = device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&pipelineState));
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create pipeline state! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return false;
    }

    SLEAK_INFO("DirectX 12 pipeline state created successfully");
    return true;
}

// -----------------------------------------------------------------------
// Render Loop
// -----------------------------------------------------------------------

void DirectX12Renderer::BeginRender() {
    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // Reset the command allocator and command list
    commandAllocator->Reset();
    commandList->Reset(commandAllocator.Get(),
                       pipelineState ? pipelineState.Get() : nullptr);

    // Set PSO if available (created by CreateShader)
    if (pipelineState) {
        commandList->SetPipelineState(pipelineState.Get());
    } else {
        SLEAK_WARN("BeginRender: pipeline state is null — nothing will draw");
    }

    // Set root signature and viewport/scissor
    commandList->SetGraphicsRootSignature(rootSignature.Get());
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    // Transition back buffer to render target state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = renderTargets[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    // Clear render target
    const float clearColor[] = {0.39f, 0.58f, 0.93f, 1.0f};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += frameIndex * rtvDescriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
        dsvHeap->GetCPUDescriptorHandleForHeapStart();

    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH,
                                       1.0f, 0, 0, nullptr);

    // Set render targets
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // Set primitive topology
    commandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind default white texture so the SRV table is always valid
    if (m_defaultTexture) {
        m_defaultTexture->Bind(0);
    }

    if (bImInitialized) {
        ID3D12DescriptorHeap* heaps[] = {imguiSrvHeap.Get()};
        commandList->SetDescriptorHeaps(1, heaps);

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }
}

void DirectX12Renderer::EndRender() {
    if (bImInitialized) {
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(),
                                       commandList.Get());
    }

    // Transition back buffer to present state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = renderTargets[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    // Close and execute the command list
    commandList->Close();
    ID3D12CommandList* ppCommandLists[] = {commandList.Get()};
    commandQueue->ExecuteCommandLists(_countof(ppCommandLists),
                                      ppCommandLists);

    // Present the frame
    swapChain->Present(1, 0);

    // Wait for the frame to finish
    const UINT64 currentFenceValue = fenceValue;
    commandQueue->Signal(fence.Get(), currentFenceValue);
    fenceValue++;

    if (fence->GetCompletedValue() < currentFenceValue) {
        fence->SetEventOnCompletion(currentFenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

void DirectX12Renderer::WaitForGPU() {
    if (!commandQueue || !fence || !fenceEvent) return;

    const UINT64 currentFenceValue = fenceValue;
    commandQueue->Signal(fence.Get(), currentFenceValue);
    fenceValue++;

    if (fence->GetCompletedValue() < currentFenceValue) {
        fence->SetEventOnCompletion(currentFenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

void DirectX12Renderer::Cleanup() {
    if (!m_Initialized) return;

    WaitForGPU();

    if (bImInitialized) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        bImInitialized = false;
    }
    imguiSrvHeap.Reset();

    m_skyboxPipelineState.Reset();
    pipelineState.Reset();
    rootSignature.Reset();
    depthStencilBuffer.Reset();
    dsvHeap.Reset();
    for (auto& rt : renderTargets) {
        rt.Reset();
    }
    rtvHeap.Reset();
    fence.Reset();
    commandList.Reset();
    commandAllocator.Reset();
    commandQueue.Reset();
    swapChain.Reset();
    device.Reset();

    m_Initialized = false;
}

void DirectX12Renderer::Resize(uint32_t width, uint32_t height) {
    if (!m_Initialized || !swapChain) return;

    WaitForGPU();

    // Release render target references
    for (auto& rt : renderTargets) {
        rt.Reset();
    }
    depthStencilBuffer.Reset();
    dsvHeap.Reset();
    rtvHeap.Reset();

    // Resize swap chain buffers
    HRESULT hr = swapChain->ResizeBuffers(FrameCount, width, height,
                                           DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to resize swap chain buffers!");
        return;
    }

    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // Recreate render target views and depth stencil
    if (!CreateRenderTargetViews() || !CreateDepthStencilView()) {
        SLEAK_ERROR("Failed to recreate views after resize!");
        return;
    }

    // Update viewport and scissor
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    scissorRect.right = width;
    scissorRect.bottom = height;

    SLEAK_INFO("DirectX 12 resized to {}x{}", width, height);
}

// -----------------------------------------------------------------------
// RenderContext: Draw Commands
// -----------------------------------------------------------------------

void DirectX12Renderer::Draw(uint32_t vertexCount) {
    commandList->DrawInstanced(vertexCount, 1, 0, 0);
}

void DirectX12Renderer::DrawIndexed(uint32_t indexCount) {
    commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
}

void DirectX12Renderer::DrawInstance(uint32_t instanceCount,
                                      uint32_t vertexPerInstance) {
    commandList->DrawInstanced(vertexPerInstance, instanceCount, 0, 0);
}

void DirectX12Renderer::DrawIndexedInstance(uint32_t instanceCount,
                                             uint32_t indexPerInstance) {
    commandList->DrawIndexedInstanced(indexPerInstance, instanceCount, 0,
                                      0, 0);
}

// -----------------------------------------------------------------------
// RenderContext: State Management
// -----------------------------------------------------------------------

void DirectX12Renderer::SetRenderFace(RenderFace face) {
    Face = face;
    ConfigureRenderFace();
}

void DirectX12Renderer::SetRenderMode(RenderMode mode) {
    Mode = mode;
    ConfigureRenderMode();
}

void DirectX12Renderer::SetViewport(float x, float y, float width,
                                     float height, float minDepth,
                                     float maxDepth) {
    viewport.TopLeftX = x;
    viewport.TopLeftY = y;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinDepth = minDepth;
    viewport.MaxDepth = maxDepth;

    scissorRect.left = static_cast<LONG>(x);
    scissorRect.top = static_cast<LONG>(y);
    scissorRect.right = static_cast<LONG>(x + width);
    scissorRect.bottom = static_cast<LONG>(y + height);

    if (commandList) {
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissorRect);
    }
}

void DirectX12Renderer::ClearRenderTarget(float r, float g, float b,
                                           float a) {
    const float clearColor[] = {r, g, b, a};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += frameIndex * rtvDescriptorSize;
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
}

void DirectX12Renderer::ClearDepthStencil(bool clearDepth,
                                           bool clearStencil, float depth,
                                           uint8_t stencil) {
    D3D12_CLEAR_FLAGS flags = {};
    if (clearDepth)
        flags = static_cast<D3D12_CLEAR_FLAGS>(
            flags | D3D12_CLEAR_FLAG_DEPTH);
    if (clearStencil)
        flags = static_cast<D3D12_CLEAR_FLAGS>(
            flags | D3D12_CLEAR_FLAG_STENCIL);

    if (flags && dsvHeap) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
            dsvHeap->GetCPUDescriptorHandleForHeapStart();
        commandList->ClearDepthStencilView(dsvHandle, flags, depth,
                                           stencil, 0, nullptr);
    }
}

// -----------------------------------------------------------------------
// RenderContext: Buffer Binding
// -----------------------------------------------------------------------

void DirectX12Renderer::BindVertexBuffer(RefPtr<BufferBase> buffer,
                                          uint32_t slot) {
    auto* dx12Buf = dynamic_cast<DirectX12Buffer*>(buffer.get());
    if (!dx12Buf) return;

    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    vbView.BufferLocation =
        dx12Buf->GetD3DBuffer()->GetGPUVirtualAddress();
    vbView.StrideInBytes = sizeof(Sleak::Vertex);
    vbView.SizeInBytes = static_cast<UINT>(dx12Buf->GetSize());

    commandList->IASetVertexBuffers(slot, 1, &vbView);
}

void DirectX12Renderer::BindIndexBuffer(RefPtr<BufferBase> buffer,
                                         uint32_t slot) {
    auto* dx12Buf = dynamic_cast<DirectX12Buffer*>(buffer.get());
    if (!dx12Buf) return;

    D3D12_INDEX_BUFFER_VIEW ibView = {};
    ibView.BufferLocation =
        dx12Buf->GetD3DBuffer()->GetGPUVirtualAddress();
    ibView.Format = DXGI_FORMAT_R32_UINT;  // IndexType is uint32_t
    ibView.SizeInBytes = static_cast<UINT>(dx12Buf->GetSize());

    commandList->IASetIndexBuffer(&ibView);
}

void DirectX12Renderer::BindConstantBuffer(RefPtr<BufferBase> buffer,
                                            uint32_t slot) {
    auto* dx12Buf = dynamic_cast<DirectX12Buffer*>(buffer.get());
    if (!dx12Buf) return;

    commandList->SetGraphicsRootConstantBufferView(
        slot, dx12Buf->GetD3DBuffer()->GetGPUVirtualAddress());
}

// -----------------------------------------------------------------------
// RenderContext: Resource Creation
// -----------------------------------------------------------------------

BufferBase* DirectX12Renderer::CreateBuffer(BufferType Type, uint32_t size,
                                             void* data) {
    assert(size > 0);
    auto* buffer = new DirectX12Buffer(device.Get(), size, Type);
    buffer->Initialize(data);

    // Execute the buffer's upload command list if it recorded any
    // copy commands (DEFAULT heap buffers with initial data).
    if (buffer->HasPendingCommands()) {
        ID3D12CommandList* ppCmdLists[] = {buffer->GetCommandList()};
        commandQueue->ExecuteCommandLists(1, ppCmdLists);
        WaitForGPU();
    }

    return buffer;
}

Shader* DirectX12Renderer::CreateShader(const std::string& shaderSource) {
    auto* shader = new DirectX12Shader(device.Get());
    if (shader->compile(shaderSource)) {
        // Create/update the PSO with this shader's compiled blobs
        if (!pipelineState && shader->getVertexShaderBlob()) {
            CreatePipelineStateFromShader(
                shader->getVertexShaderBlob(),
                shader->getPixelShaderBlob());
        }
        return shader;
    }
    delete shader;
    return nullptr;
}

Texture* DirectX12Renderer::CreateTexture(
    const std::string& TexturePath) {
    auto* texture =
        new DirectX12Texture(device.Get(), commandQueue.Get(),
                             commandList.Get());
    if (!texture->LoadFromFile(TexturePath)) {
        delete texture;
        return nullptr;
    }
    return texture;
}

Texture* DirectX12Renderer::CreateTextureFromData(uint32_t width,
                                                    uint32_t height,
                                                    void* data) {
    auto* texture =
        new DirectX12Texture(device.Get(), commandQueue.Get(),
                             commandList.Get());
    if (!texture->LoadFromMemory(data, width, height,
                                 TextureFormat::RGBA8)) {
        delete texture;
        return nullptr;
    }
    return texture;
}

Texture* DirectX12Renderer::CreateCubemapTexture(
    const std::array<std::string, 6>& facePaths) {
    auto* texture = new DirectX12CubemapTexture(device.Get(),
                                                 commandQueue.Get());
    if (texture->LoadCubemap(facePaths)) {
        return texture;
    }
    delete texture;
    return nullptr;
}

Texture* DirectX12Renderer::CreateCubemapTextureFromPanorama(
    const std::string& panoramaPath) {
    auto* texture = new DirectX12CubemapTexture(device.Get(),
                                                 commandQueue.Get());
    if (texture->LoadEquirectangular(panoramaPath)) {
        return texture;
    }
    delete texture;
    return nullptr;
}

void DirectX12Renderer::BindTexture(RefPtr<Sleak::Texture> texture,
                                     uint32_t slot) {
    if (!texture.IsValid() || !commandList) return;

    // Try DX12 cubemap texture first
    auto* cubemap =
        dynamic_cast<DirectX12CubemapTexture*>(texture.get());
    if (cubemap) {
        cubemap->BindToCommandList(commandList.Get(), 1);
        return;
    }

    // Try regular DX12 texture
    auto* dx12Tex = dynamic_cast<DirectX12Texture*>(texture.get());
    if (dx12Tex) {
        dx12Tex->BindToCommandList(commandList.Get(), 1);
    }
}

bool DirectX12Renderer::CreateSkyboxPipelineState() {
    if (m_skyboxPipelineState) return true;
    if (!pipelineState) return false;

    // Get the main PSO's description by compiling the skybox shader
    // We need the same VS/PS blobs used for the main PSO
    // Compile the skybox shader directly
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    HRESULT hr = D3DCompileFromFile(
        L"assets/shaders/skybox_dx12.hlsl", nullptr, nullptr,
        "VS_Main", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            SLEAK_ERROR("Skybox VS compile error: {}",
                        (char*)errorBlob->GetBufferPointer());
        }
        SLEAK_WARN("Failed to compile skybox VS, using main PSO blobs");
    }

    hr = D3DCompileFromFile(
        L"assets/shaders/skybox_dx12.hlsl", nullptr, nullptr,
        "PS_Main", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            SLEAK_ERROR("Skybox PS compile error: {}",
                        (char*)errorBlob->GetBufferPointer());
        }
        SLEAK_WARN("Failed to compile skybox PS");
    }

    if (!vsBlob || !psBlob) return false;

    // Input layout matching Sleak::Vertex
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, px)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, nx)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, tx)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, r)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
         static_cast<UINT>(offsetof(Sleak::Vertex, u)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};

    // Rasterizer: no culling for skybox
    D3D12_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = 0;
    rasterDesc.DepthBiasClamp = 0.0f;
    rasterDesc.SlopeScaledDepthBias = 0.0f;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;
    rasterDesc.ForcedSampleCount = 0;
    rasterDesc.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    psoDesc.RasterizerState = rasterDesc;

    // Blend state (same as main)
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC rtBlendDesc = {};
    rtBlendDesc.BlendEnable = TRUE;
    rtBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rtBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rtBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        blendDesc.RenderTarget[i] = rtBlendDesc;
    psoDesc.BlendState = blendDesc;

    // Depth stencil: depth write OFF, LESS_EQUAL compare
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    depthStencilDesc.StencilEnable = FALSE;
    psoDesc.DepthStencilState = depthStencilDesc;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    hr = device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&m_skyboxPipelineState));
    if (FAILED(hr)) {
        SLEAK_ERROR(
            "Failed to create skybox pipeline state! HRESULT: 0x{:08X}",
            static_cast<unsigned int>(hr));
        return false;
    }

    SLEAK_INFO("DirectX 12 skybox pipeline state created successfully");
    return true;
}

void DirectX12Renderer::BeginSkyboxPass() {
    if (!CreateSkyboxPipelineState()) {
        SLEAK_WARN("BeginSkyboxPass: failed to create skybox PSO");
        return;
    }
    commandList->SetPipelineState(m_skyboxPipelineState.Get());
}

void DirectX12Renderer::EndSkyboxPass() {
    if (pipelineState) {
        commandList->SetPipelineState(pipelineState.Get());
    }
}

// -----------------------------------------------------------------------
// Pipeline State Configuration (DX12 requires PSO recreation)
// -----------------------------------------------------------------------

void DirectX12Renderer::ConfigureRenderMode() {
    // In DX12, fill mode and topology are baked into the PSO.
    // For now, we only support the default solid fill + triangle list.
    // A full implementation would cache multiple PSO variants.
}

void DirectX12Renderer::ConfigureRenderFace() {
    // In DX12, cull mode is baked into the PSO.
    // A full implementation would cache multiple PSO variants.
}

// -----------------------------------------------------------------------
// Device Enumeration
// -----------------------------------------------------------------------

void DirectX12Renderer::EnumerateDevices(
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory) {
    UINT adapter_index = 0;
    IDXGIAdapter1* tempadapter;
    while (factory->EnumAdapters1(adapter_index, &tempadapter) !=
           DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        tempadapter->GetDesc1(&desc);

#ifdef _DEBUG
        std::wstring wgpu_name = L"Found GPU: ";
        wgpu_name += desc.Description;
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        std::string gpu_name = converter.to_bytes(wgpu_name);
        SLEAK_INFO(gpu_name);
#endif

        if (adapter == nullptr &&
            (desc.VendorId == 0x10DE || desc.VendorId == 0x1002)) {
            adapter.Attach(tempadapter);
        } else {
            tempadapter->Release();
        }

        adapter_index++;
    }

    SLEAK_INFO("Found {} GPUs", adapter_index);
}

bool DirectX12Renderer::IsSupport() {
    Microsoft::WRL::ComPtr<ID3D12Device> testDevice;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&testDevice));
    return SUCCEEDED(hr);
}

bool DirectX12Renderer::CreateImGUI() {
    if (!device || !commandQueue)
        return false;

    // Create a dedicated SRV descriptor heap for ImGUI
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 1;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&imguiSrvHeap)))) {
        SLEAK_ERROR("Failed to create ImGUI SRV heap!");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForD3D(window->GetSDLWindow()))
        return false;

    if (!ImGui_ImplDX12_Init(
            device.Get(), FrameCount, DXGI_FORMAT_R8G8B8A8_UNORM,
            imguiSrvHeap.Get(),
            imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
            imguiSrvHeap->GetGPUDescriptorHandleForHeapStart()))
        return false;

    bImInitialized = true;
    return true;
}

}  // namespace RenderEngine
}  // namespace Sleak

#endif
