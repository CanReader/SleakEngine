#include "../../include/private/Graphics/DirectX/DirectX12Renderer.hpp"
#ifdef PLATFORM_WIN

#include <Window.hpp>
#include <SDL3/SDL_system.h>
#include <Graphics/Vertex.hpp>
#include <Graphics/DirectX/DirectX12CubemapTexture.hpp>
#include <Graphics/ConstantBuffer.hpp>
#include <Logger.hpp>
#include <stdexcept>
#include <string>
#include <locale>
#include <codecvt>
#include <d3dcompiler.h>
#include <Graphics/DirectX/DirectX12Buffer.hpp>

namespace Sleak {
namespace RenderEngine {

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
            if (!tex->LoadFromMemory(data, w, h, fmt)) { delete tex; return nullptr; }
            UINT slot = AllocateSRVSlot();
            tex->CreateSRVIntoHandle(DXGI_FORMAT_R8G8B8A8_UNORM,
                GetSharedSrvCPUHandle(slot));
            tex->SetSharedSrvGPUHandle(GetSharedSrvGPUHandle(slot));
            return tex;
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
    if (!CreateSharedSrvHeap()) return false;
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
        } else {
            UINT slot = AllocateSRVSlot();
            m_defaultTexture->CreateSRVIntoHandle(DXGI_FORMAT_R8G8B8A8_UNORM,
                GetSharedSrvCPUHandle(slot));
            m_defaultTexture->SetSharedSrvGPUHandle(GetSharedSrvGPUHandle(slot));
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

    SetPerformanceCounter(true);

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
    // Create one command allocator per frame-in-flight so the CPU can
    // record frame N+1 while the GPU is still executing frame N.
    for (UINT i = 0; i < FrameCount; i++) {
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&commandAllocators[i])))) {
            SLEAK_ERROR("Failed to create command allocator {}!", i);
            return false;
        }
    }

    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[0].Get(),
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
    for (UINT i = 0; i < FrameCount; i++)
        fenceValues[i] = 0;
    return true;
}

bool DirectX12Renderer::CreateRootSignature() {
    // Parameter 0: CBV at register(b0) — transform (vertex shader)
    // Parameter 1: CBV at register(b1) — material  (all shaders)
    // Parameter 2: SRV descriptor table at register(t0) — texture (pixel shader)
    // Parameter 3: CBV at register(b2) — lighting/fog (pixel shader)
    D3D12_ROOT_PARAMETER rootParams[4] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[3].Descriptor.ShaderRegister = 2;
    rootParams[3].Descriptor.RegisterSpace = 0;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers
    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};

    // s0: POINT/CLAMP — block textures (nearest-neighbor for pixel art)
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].MipLODBias = 0.0f;
    staticSamplers[0].MaxAnisotropy = 1;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[0].BorderColor =
        D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSamplers[0].MinLOD = 0.0f;
    staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].RegisterSpace = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1: LINEAR/WRAP — skybox cubemap (smooth filtering)
    staticSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].MipLODBias = 0.0f;
    staticSamplers[1].MaxAnisotropy = 1;
    staticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[1].BorderColor =
        D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSamplers[1].MinLOD = 0.0f;
    staticSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[1].ShaderRegister = 1;
    staticSamplers[1].RegisterSpace = 0;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 4;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 2;
    rootSigDesc.pStaticSamplers = staticSamplers;
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

    // Blend state — opaque (no blending)
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC rtBlendDesc = {};
    rtBlendDesc.BlendEnable = FALSE;
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

void DirectX12Renderer::BeginRender() {
    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // Wait for ALL pending GPU work to complete before starting a new frame.
    // This prevents race conditions on shared constant buffers: without this,
    // only the same-slot frame (N-2) is waited on, but frame N-1 may still be
    // reading constant buffer data that the CPU is about to overwrite.
    UINT64 waitValue = 0;
    for (UINT i = 0; i < FrameCount; i++) {
        if (fenceValues[i] > waitValue) waitValue = fenceValues[i];
    }
    if (fence->GetCompletedValue() < waitValue) {
        fence->SetEventOnCompletion(waitValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }

    // Process deferred GPU resource deletions now that GPU is idle
    DirectX12Buffer::ProcessDeferredCleanup();

    // Reset the command allocator and command list for this frame
    commandAllocators[frameIndex]->Reset();
    commandList->Reset(commandAllocators[frameIndex].Get(),
                       pipelineState ? pipelineState.Get() : nullptr);

    // Set PSO if available (created by CreateShader)
    if (pipelineState) {
        commandList->SetPipelineState(pipelineState.Get());
    } else {
        SLEAK_WARN("BeginRender: pipeline state is null — nothing will draw");
    }

    // Set root signature and viewport/scissor
    commandList->SetGraphicsRootSignature(rootSignature.Get());

    // Bind the shared SRV heap ONCE for the entire frame — never switch heaps
    ID3D12DescriptorHeap* srvHeaps[] = {m_sharedSrvHeap.Get()};
    commandList->SetDescriptorHeaps(1, srvHeaps);

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

    // Bind light/fog constant buffer at root parameter 3 (register b2)
    if (m_lightUBOCreated && m_lightUBO) {
        commandList->SetGraphicsRootConstantBufferView(
            3, m_lightUBO->GetGPUVirtualAddress());
    }

    bImFrameActive = false;
    if (bImInitialized) {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        // Guard against first frames before SDL has reported a valid window size
        auto& io = ImGui::GetIO();
        if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
            ImGui::NewFrame();
            bImFrameActive = true;
        }
    }
}

void DirectX12Renderer::EndRender() {
    if (bImFrameActive) {
        ImGui::Render();
        // Shared SRV heap is already bound from BeginRender — no heap switch needed
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

    swapChain->Present(m_vsync ? 1 : 0, 0);

    // Signal the fence for this frame — do NOT wait here.
    // The wait happens in BeginRender() when we need to reuse this
    // frame's command allocator, allowing CPU/GPU overlap.
    fenceValues[frameIndex]++;
    commandQueue->Signal(fence.Get(), fenceValues[frameIndex]);

    UpdateFrameMetrics();
}

void DirectX12Renderer::WaitForGPU() {
    if (!commandQueue || !fence || !fenceEvent) return;

    // Find the highest fence value across all frames and wait for it
    UINT64 maxFence = 0;
    for (UINT i = 0; i < FrameCount; i++) {
        if (fenceValues[i] > maxFence) maxFence = fenceValues[i];
    }
    const UINT64 waitValue = maxFence + 1;
    commandQueue->Signal(fence.Get(), waitValue);
    for (UINT i = 0; i < FrameCount; i++)
        fenceValues[i] = waitValue;

    if (fence->GetCompletedValue() < waitValue) {
        fence->SetEventOnCompletion(waitValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

bool DirectX12Renderer::CreateSharedSrvHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = MAX_SRV_DESCRIPTORS;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(
        &heapDesc, IID_PPV_ARGS(&m_sharedSrvHeap));
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create shared SRV descriptor heap!");
        return false;
    }
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_nextSrvSlot = 0;
    return true;
}

UINT DirectX12Renderer::AllocateSRVSlot() {
    if (m_nextSrvSlot >= MAX_SRV_DESCRIPTORS) {
        SLEAK_ERROR("Shared SRV heap full! Increase MAX_SRV_DESCRIPTORS.");
        return 0;
    }
    return m_nextSrvSlot++;
}

D3D12_CPU_DESCRIPTOR_HANDLE DirectX12Renderer::GetSharedSrvCPUHandle(UINT slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_sharedSrvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * m_srvDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DirectX12Renderer::GetSharedSrvGPUHandle(UINT slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_sharedSrvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(slot) * m_srvDescriptorSize;
    return handle;
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
    if (m_lightUBO && m_lightUBOMapped) {
        m_lightUBO->Unmap(0, nullptr);
        m_lightUBOMapped = nullptr;
    }
    m_lightUBO.Reset();
    m_lightUBOCreated = false;

    imguiSrvHeap.Reset();
    m_sharedSrvHeap.Reset();

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
    for (UINT i = 0; i < FrameCount; i++)
        commandAllocators[i].Reset();
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

void DirectX12Renderer::Draw(uint32_t vertexCount) {
    commandList->DrawInstanced(vertexCount, 1, 0, 0);
    DrawnVertices += vertexCount;
    DrawnTriangles += vertexCount / 3;
}

void DirectX12Renderer::DrawIndexed(uint32_t indexCount) {
    commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    DrawnVertices += indexCount;
    DrawnTriangles += indexCount / 3;
}

void DirectX12Renderer::DrawInstance(uint32_t instanceCount,
                                      uint32_t vertexPerInstance) {
    commandList->DrawInstanced(vertexPerInstance, instanceCount, 0, 0);
    DrawnVertices += vertexPerInstance * instanceCount;
    DrawnTriangles += (vertexPerInstance / 3) * instanceCount;
}

void DirectX12Renderer::DrawIndexedInstance(uint32_t instanceCount,
                                             uint32_t indexPerInstance) {
    commandList->DrawIndexedInstanced(indexPerInstance, instanceCount, 0,
                                      0, 0);
    DrawnVertices += indexPerInstance * instanceCount;
    DrawnTriangles += (indexPerInstance / 3) * instanceCount;
}

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

void DirectX12Renderer::BindVertexBuffer(RefPtr<BufferBase> buffer,
                                          uint32_t slot) {
    auto* dx12Buf = static_cast<DirectX12Buffer*>(buffer.get());
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
    auto* dx12Buf = static_cast<DirectX12Buffer*>(buffer.get());
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
    auto* dx12Buf = static_cast<DirectX12Buffer*>(buffer.get());
    if (!dx12Buf) return;

    commandList->SetGraphicsRootConstantBufferView(
        slot, dx12Buf->GetD3DBuffer()->GetGPUVirtualAddress());
}

BufferBase* DirectX12Renderer::CreateBuffer(BufferType Type, uint32_t size,
                                             void* data) {
    assert(size > 0);
    auto* buffer = new DirectX12Buffer(device.Get(), commandQueue.Get(), size, Type);
    buffer->Initialize(data);

    // Execute the buffer's upload command list if it recorded any
    // copy commands (DEFAULT heap buffers with initial data).
    // Do NOT call WaitForGPU() here — the upload runs on the same
    // command queue, so GPU FIFO ordering guarantees the copy
    // completes before subsequent render commands execute.
    if (buffer->HasPendingCommands()) {
        ID3D12CommandList* ppCmdLists[] = {buffer->GetCommandList()};
        commandQueue->ExecuteCommandLists(1, ppCmdLists);
    }

    return buffer;
}

Shader* DirectX12Renderer::CreateShader(const std::string& shaderSource) {
    auto* shader = new DirectX12Shader(device.Get());
    if (shader->compile(shaderSource)) {
        // Create a per-shader PSO so each shader gets its own pipeline
        if (shader->getVertexShaderBlob()) {
            CreatePipelineStateFromShader(
                shader->getVertexShaderBlob(),
                shader->getPixelShaderBlob());
            // Store the newly created PSO on this shader
            shader->SetPipelineState(pipelineState);
        }
        // Give the shader access to the command list for bind()
        shader->SetCommandList(commandList.Get());
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
    // Place the SRV into the shared heap so we never switch heaps at draw time
    UINT slot = AllocateSRVSlot();
    texture->CreateSRVIntoHandle(
        texture->GetFormat() == TextureFormat::RGBA8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM,
        GetSharedSrvCPUHandle(slot));
    texture->SetSharedSrvGPUHandle(GetSharedSrvGPUHandle(slot));
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
    // Place the SRV into the shared heap
    UINT slot = AllocateSRVSlot();
    texture->CreateSRVIntoHandle(DXGI_FORMAT_R8G8B8A8_UNORM,
        GetSharedSrvCPUHandle(slot));
    texture->SetSharedSrvGPUHandle(GetSharedSrvGPUHandle(slot));
    return texture;
}

Texture* DirectX12Renderer::CreateCubemapTexture(
    const std::array<std::string, 6>& facePaths) {
    auto* texture = new DirectX12CubemapTexture(device.Get(),
                                                 commandQueue.Get());
    if (!texture->LoadCubemap(facePaths)) {
        delete texture;
        return nullptr;
    }
    UINT slot = AllocateSRVSlot();
    texture->CreateSRVIntoHandle(GetSharedSrvCPUHandle(slot));
    texture->SetSharedSrvGPUHandle(GetSharedSrvGPUHandle(slot));
    return texture;
}

Texture* DirectX12Renderer::CreateCubemapTextureFromPanorama(
    const std::string& panoramaPath) {
    auto* texture = new DirectX12CubemapTexture(device.Get(),
                                                 commandQueue.Get());
    if (!texture->LoadEquirectangular(panoramaPath)) {
        delete texture;
        return nullptr;
    }
    UINT slot = AllocateSRVSlot();
    texture->CreateSRVIntoHandle(GetSharedSrvCPUHandle(slot));
    texture->SetSharedSrvGPUHandle(GetSharedSrvGPUHandle(slot));
    return texture;
}

void DirectX12Renderer::BindTexture(RefPtr<Sleak::Texture> texture,
                                     uint32_t slot) {
    if (!texture.IsValid() || !commandList) return;

    if (texture->GetType() == TextureType::TextureCube) {
        static_cast<DirectX12CubemapTexture*>(texture.get())
            ->BindToCommandList(commandList.Get(), 2);
    } else {
        static_cast<DirectX12Texture*>(texture.get())
            ->BindToCommandList(commandList.Get(), 2);
    }
}

void DirectX12Renderer::BindTextureRaw(Sleak::Texture* texture, uint32_t slot) {
    if (!texture || !commandList) return;

    if (texture->GetType() == TextureType::TextureCube) {
        static_cast<DirectX12CubemapTexture*>(texture)
            ->BindToCommandList(commandList.Get(), 2);
        return;
    }

    auto* dx12Tex = static_cast<DirectX12Texture*>(texture);
    if (dx12Tex) {
        dx12Tex->BindToCommandList(commandList.Get(), 2);
    }
}

bool DirectX12Renderer::CreateLightUBO() {
    if (m_lightUBOCreated) return true;

    // 256-byte aligned size (CB requirement)
    const UINT uboSize = (sizeof(RenderEngine::ShadowLightUBO) + 255) & ~255;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = uboSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_lightUBO));
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create light UBO! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return false;
    }

    hr = m_lightUBO->Map(0, nullptr, &m_lightUBOMapped);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to map light UBO!");
        return false;
    }

    memset(m_lightUBOMapped, 0, uboSize);
    m_lightUBOCreated = true;
    return true;
}

void DirectX12Renderer::UpdateShadowLightUBO(const void* data, uint32_t size) {
    if (!m_lightUBOCreated && !CreateLightUBO()) return;
    if (!data) return;

    uint32_t copySize = size;
    if (copySize > sizeof(RenderEngine::ShadowLightUBO))
        copySize = sizeof(RenderEngine::ShadowLightUBO);
    memcpy(m_lightUBOMapped, data, copySize);
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

    // Rasterizer: no culling for skybox, depth clip OFF to avoid
    // clipping at the z=1.0 boundary produced by the .xyww trick
    D3D12_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthClipEnable = FALSE;
    psoDesc.RasterizerState = rasterDesc;

    // Blend state — opaque (skybox is fully opaque)
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC rtBlendDesc = {};
    rtBlendDesc.BlendEnable = FALSE;
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

bool DirectX12Renderer::CreateDebugLinePipelineState() {
    if (m_debugLinePipelineState) return true;
    if (!pipelineState) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    HRESULT hr = D3DCompileFromFile(
        L"assets/shaders/debug_line_dx12.hlsl", nullptr, nullptr,
        "VS_Main", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob)
            SLEAK_ERROR("Debug line VS compile error: {}",
                        (char*)errorBlob->GetBufferPointer());
        return false;
    }

    hr = D3DCompileFromFile(
        L"assets/shaders/debug_line_dx12.hlsl", nullptr, nullptr,
        "PS_Main", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob)
            SLEAK_ERROR("Debug line PS compile error: {}",
                        (char*)errorBlob->GetBufferPointer());
        return false;
    }

    if (!vsBlob || !psBlob) return false;

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

    D3D12_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthClipEnable = TRUE;
    psoDesc.RasterizerState = rasterDesc;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC rtBlendDesc = {};
    rtBlendDesc.BlendEnable = FALSE;
    rtBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        blendDesc.RenderTarget[i] = rtBlendDesc;
    psoDesc.BlendState = blendDesc;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    depthStencilDesc.StencilEnable = FALSE;
    psoDesc.DepthStencilState = depthStencilDesc;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    hr = device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&m_debugLinePipelineState));
    if (FAILED(hr)) {
        SLEAK_ERROR(
            "Failed to create debug line pipeline state! HRESULT: 0x{:08X}",
            static_cast<unsigned int>(hr));
        return false;
    }

    SLEAK_INFO("DirectX 12 debug line pipeline state created successfully");
    return true;
}

void DirectX12Renderer::BeginDebugLinePass() {
    if (!CreateDebugLinePipelineState()) {
        SLEAK_WARN("BeginDebugLinePass: failed to create debug line PSO");
        return;
    }
    commandList->SetPipelineState(m_debugLinePipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
}

void DirectX12Renderer::EndDebugLinePass() {
    if (pipelineState) {
        commandList->SetPipelineState(pipelineState.Get());
    }
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void DirectX12Renderer::ConfigureRenderMode() {
    // In DX12, fill mode and topology are baked into the PSO.
    // For now, we only support the default solid fill + triangle list.
    // A full implementation would cache multiple PSO variants.
}

void DirectX12Renderer::ConfigureRenderFace() {
    // In DX12, cull mode is baked into the PSO.
    // A full implementation would cache multiple PSO variants.
}

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
    if (!device || !commandQueue || !m_sharedSrvHeap)
        return false;

    // Allocate a slot in the shared SRV heap for ImGui's font texture
    UINT imguiSlot = AllocateSRVSlot();
    D3D12_CPU_DESCRIPTOR_HANDLE imguiCpuHandle = GetSharedSrvCPUHandle(imguiSlot);
    D3D12_GPU_DESCRIPTOR_HANDLE imguiGpuHandle = GetSharedSrvGPUHandle(imguiSlot);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForD3D(window->GetSDLWindow()))
        return false;

    if (!ImGui_ImplDX12_Init(
            device.Get(), FrameCount, DXGI_FORMAT_R8G8B8A8_UNORM,
            m_sharedSrvHeap.Get(),
            imguiCpuHandle,
            imguiGpuHandle))
        return false;

    bImInitialized = true;
    return true;
}

}  // namespace RenderEngine
}  // namespace Sleak

#endif
