#include "../../include/private/Graphics/DirectX12/DirectX12Renderer.hpp"
#ifdef PLATFORM_WIN

#include <Core/Window.hpp>
#include <SDL3/SDL_system.h>
#include <Runtime/MeshData.hpp>
#include <Graphics/DirectX12/DirectX12CubemapTexture.hpp>
#include <Graphics/Common/ConstantBuffer.hpp>
#include <Core/Logger.hpp>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <locale>
#include <codecvt>
#include <d3dcompiler.h>
#include <Graphics/DirectX12/DirectX12Buffer.hpp>
#include "Graphics/Common/RenderCommandQueue.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {
const D3D12_INPUT_ELEMENT_DESC kVertexInputLayout[] = {
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

uint64_t HashBytecode(ID3DBlob* blob) {
    if (!blob) return 0;
    const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
    uint64_t hash = 14695981039346656037ull;
    for (SIZE_T i = 0; i < blob->GetBufferSize(); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

Microsoft::WRL::ComPtr<ID3DBlob> CompileStage(const wchar_t* path,
                                              const char* entry,
                                              const char* profile) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errorBlob;
    HRESULT hr = D3DCompileFromFile(path, nullptr, nullptr, entry, profile, 0,
                                    0, &blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            SLEAK_ERROR("{} ({}) compile error: {}", entry, profile,
                        (char*)errorBlob->GetBufferPointer());
        }
        return nullptr;
    }
    return blob;
}
}  // namespace

/// Registers this backend's buffer/shader/texture factories with ResourceManager.
DirectX12Renderer::DirectX12Renderer(Window* window) : window(window) {
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent == nullptr) {
        throw std::runtime_error("Failed to create fence event.");
    }
    this->Type = RendererType::DirectX12;
    m_lightData.assign(sizeof(RenderEngine::ShadowLightUBO), 0);

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
        [this](const void* data, uint32_t w, uint32_t h, TextureFormat fmt, uint32_t) -> Texture* {
            auto* tex = new DirectX12Texture(device.Get(), commandQueue.Get(),
                                             commandList.Get(), &m_uploader);
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
    m_uploadRing.Initialize(device.Get(), FrameCount, 64 * 1024);
    if (!m_uploader.Initialize(device.Get(), commandQueue.Get())) return false;
    if (!CreateRootSignature()) return false;
    if (!CreateSharedSrvHeap()) return false;
    // PSO is created lazily when CreateShader() is called

    // Create a 1x1 white default texture so the SRV table is always valid
    {
        uint32_t whitePixel = 0xFFFFFFFF;  // RGBA(255,255,255,255)
        m_defaultTexture = new DirectX12Texture(
            device.Get(), commandQueue.Get(), commandList.Get(), &m_uploader);
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
    m_fenceValue = 0;
    for (UINT i = 0; i < FrameCount; i++)
        fenceValues[i] = 0;
    return true;
}

bool DirectX12Renderer::CreateRootSignature() {
    // Parameter 0: CBV at register(b0) — transform (vertex shader)
    // Parameter 1: CBV at register(b1) — material  (all shaders)
    // Parameter 2: SRV descriptor table at register(t0) — texture (pixel shader)
    // Parameter 3: CBV at register(b2) — lighting/fog (all shaders, VS needs LightVP)
    // Parameter 4: SRV descriptor table at register(t3) — shadow map (pixel shader)
    D3D12_ROOT_PARAMETER rootParams[5] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE shadowSrvRange = {};
    shadowSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowSrvRange.NumDescriptors = 1;
    shadowSrvRange.BaseShaderRegister = 3;
    shadowSrvRange.RegisterSpace = 0;
    shadowSrvRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[4].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges = &shadowSrvRange;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers
    D3D12_STATIC_SAMPLER_DESC staticSamplers[3] = {};

    // s0: ANISOTROPIC 16x — surface textures (high quality filtering)
    staticSamplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].MipLODBias = 0.0f;
    staticSamplers[0].MaxAnisotropy = 16;
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

    // s3: COMPARISON sampler — shadow map PCF
    staticSamplers[2].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    staticSamplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[2].MipLODBias = 0.0f;
    staticSamplers[2].MaxAnisotropy = 1;
    staticSamplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSamplers[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    staticSamplers[2].MinLOD = 0.0f;
    staticSamplers[2].MaxLOD = 0.0f;
    staticSamplers[2].ShaderRegister = 3;
    staticSamplers[2].RegisterSpace = 0;
    staticSamplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 5;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 3;
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

ID3D12PipelineState* DirectX12Renderer::GetPipeline(const PipelineKey& key,
                                                    ID3DBlob* vs,
                                                    ID3DBlob* ps) {
    for (auto& [cachedKey, pso] : m_pipelineCache) {
        if (cachedKey == key) return pso.Get();
    }
    if (!vs || key.vertexFormat != 0) return nullptr;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {kVertexInputLayout, _countof(kVertexInputLayout)};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    if (ps) psoDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

    D3D12_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = key.cullMode;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = key.depthBias;
    rasterDesc.DepthBiasClamp = key.depthBiasClamp;
    rasterDesc.SlopeScaledDepthBias = key.slopeScaledDepthBias;
    rasterDesc.DepthClipEnable = key.depthClip;
    rasterDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    psoDesc.RasterizerState = rasterDesc;

    const bool hasColor = key.rtvFormat != DXGI_FORMAT_UNKNOWN;
    D3D12_RENDER_TARGET_BLEND_DESC rtBlend = {};
    rtBlend.BlendEnable = key.blendEnable;
    rtBlend.SrcBlend =
        key.blendEnable ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
    rtBlend.DestBlend =
        key.blendEnable ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha =
        key.blendEnable ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = hasColor ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        psoDesc.BlendState.RenderTarget[i] = rtBlend;

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = key.depthWrite;
    psoDesc.DepthStencilState.DepthFunc = key.depthFunc;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = key.topology;
    psoDesc.NumRenderTargets = hasColor ? 1 : 0;
    psoDesc.RTVFormats[0] = key.rtvFormat;
    psoDesc.DSVFormat = key.dsvFormat;
    psoDesc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    HRESULT hr =
        device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create pipeline state! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return nullptr;
    }

    m_pipelineCache.emplace_back(key, pso);
    return pso.Get();
}

bool DirectX12Renderer::CreateShadowPassPSO() {
    if (m_shadowPassPSO) return true;

    if (!m_shadowVSCompiled) {
        m_shadowVSCompiled = true;
        m_shadowVSBlob = CompileStage(
            L"assets/shaders/default_shader_dx12.hlsl", "VS_Main", "vs_5_0");
    }
    ID3DBlob* vs = m_shadowVSBlob ? m_shadowVSBlob.Get() : m_cachedVSBlob.Get();
    if (!vs) {
        SLEAK_WARN("CreateShadowPassPSO: no vertex shader available yet");
        return false;
    }

    PipelineKey key;
    key.vsHash = HashBytecode(vs);
    key.cullMode = D3D12_CULL_MODE_NONE;
    key.depthBias = 1000;
    key.depthBiasClamp = 0.01f;
    key.slopeScaledDepthBias = 2.0f;
    key.rtvFormat = DXGI_FORMAT_UNKNOWN;

    m_shadowPassPSO = GetPipeline(key, vs, nullptr);
    if (!m_shadowPassPSO) return false;
    SLEAK_INFO("D3D12 shadow pass PSO created");
    return true;
}

void DirectX12Renderer::BeginRender() {
    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // Commit staged lightVP before shadow pass so shadow + main agree.
    if (m_hasPendingLightVP) {
        memcpy(m_lightVP, m_pendingLightVP, sizeof(m_lightVP));
    }

    // Only the frame that last recorded into this slot has to be finished.
    // Everything the CPU rewrites per frame lives in per-slot memory.
    const UINT64 slotFence = fenceValues[frameIndex];
    if (fence->GetCompletedValue() < slotFence) {
        fence->SetEventOnCompletion(slotFence, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }

    DirectX12Buffer::ProcessDeferredCleanup(fence->GetCompletedValue());
    m_uploader.Retire();
    m_uploader.Submit();
    m_uploadRing.BeginFrame(frameIndex);
    ++m_frameSerial;
    m_frameActive = true;
    m_shaderPipeline = nullptr;
    m_passPipeline = nullptr;

    // Reset the command allocator and command list for this frame
    commandAllocators[frameIndex]->Reset();
    commandList->Reset(commandAllocators[frameIndex].Get(),
                       m_defaultPipelineState);

    if (!m_defaultPipelineState) {
        SLEAK_WARN("BeginRender: no pipeline state yet, nothing will draw");
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

    // Shadow pass before main rendering
    if (m_shadowPassEnabled) {
        RenderShadowPass();
        if (m_defaultPipelineState)
            commandList->SetPipelineState(m_defaultPipelineState);
    }

    // Set render targets
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
    // Restore viewport/scissor after shadow pass
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    // Set primitive topology
    commandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Re-set root signature after shadow pass (SetGraphicsRootSignature
    // invalidates all root parameter bindings, so we must rebind everything).
    commandList->SetGraphicsRootSignature(rootSignature.Get());
    {
        ID3D12DescriptorHeap* heaps[] = {m_sharedSrvHeap.Get()};
        commandList->SetDescriptorHeaps(1, heaps);
    }

    // Root param 2: SRV table at t0 — default white texture
    if (m_defaultTexture) {
        m_defaultTexture->Bind(0);
    }

    // Root param 3: CBV at b2, light/shadow constants
    BindLightConstants();

    // Root param 4: SRV table at t3 — shadow map
    if (m_shadowMapCreated) {
        commandList->SetGraphicsRootDescriptorTable(
            4, GetSharedSrvGPUHandle(m_shadowSrvIndex));
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

    // Uploads recorded this frame must land before the draws that use them
    commandList->Close();
    m_uploader.Submit();
    ID3D12CommandList* ppCommandLists[] = {commandList.Get()};
    commandQueue->ExecuteCommandLists(_countof(ppCommandLists),
                                      ppCommandLists);

    swapChain->Present(m_vsync ? 1 : 0, 0);

    // No wait here: BeginRender waits for this value only when the slot
    // comes around again.
    fenceValues[frameIndex] = ++m_fenceValue;
    commandQueue->Signal(fence.Get(), m_fenceValue);
    DirectX12Buffer::TagDeferredCleanup(m_fenceValue);
    m_frameActive = false;

    UpdateFrameMetrics();
}

void DirectX12Renderer::WaitForGPU() {
    if (!commandQueue || !fence || !fenceEvent) return;

    m_uploader.Submit();
    const UINT64 waitValue = ++m_fenceValue;
    commandQueue->Signal(fence.Get(), waitValue);

    if (fence->GetCompletedValue() < waitValue) {
        fence->SetEventOnCompletion(waitValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    m_uploader.Retire();
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
    DirectX12Buffer::TagDeferredCleanup(m_fenceValue);
    DirectX12Buffer::ProcessDeferredCleanup(UINT64_MAX);

    if (bImInitialized) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        bImInitialized = false;
    }
    m_uploader.Release();
    m_uploadRing.Release();

    imguiSrvHeap.Reset();
    m_sharedSrvHeap.Reset();

    m_skyboxPipelineState = nullptr;
    m_debugLinePipelineState = nullptr;
    m_shadowPassPSO = nullptr;
    m_defaultPipelineState = nullptr;
    m_shaderPipeline = nullptr;
    m_passPipeline = nullptr;
    m_pipelineCache.clear();
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

void DirectX12Renderer::Draw(uint32_t vertexCount) { Draw(vertexCount, 0); }

void DirectX12Renderer::Draw(uint32_t vertexCount, uint32_t firstVertex) {
    commandList->DrawInstanced(vertexCount, 1, firstVertex, 0);
    DrawnVertices += vertexCount;
    DrawnTriangles += vertexCount / 3;
}

void DirectX12Renderer::DrawIndexed(uint32_t indexCount) {
    DrawIndexed(indexCount, 0, 0);
}

void DirectX12Renderer::DrawIndexed(uint32_t indexCount, uint32_t firstIndex,
                                    int32_t baseVertex) {
    commandList->DrawIndexedInstanced(indexCount, 1, firstIndex, baseVertex, 0);
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
    DrawIndexedInstance(instanceCount, indexPerInstance, 0, 0);
}

void DirectX12Renderer::DrawIndexedInstance(uint32_t instanceCount,
                                            uint32_t indexPerInstance,
                                            uint32_t firstIndex,
                                            int32_t baseVertex) {
    commandList->DrawIndexedInstanced(indexPerInstance, instanceCount,
                                      firstIndex, baseVertex, 0);
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

    // Shadow pass: use a dedicated shadow CB with LightVP*World for slot 0
    if (m_inShadowPass && slot == 0 && dx12Buf->GetCPUShadowCopySize() >= 128) {
        const float* srcWorld = reinterpret_cast<const float*>(
            static_cast<const char*>(dx12Buf->GetCPUShadowCopy()) + 64);

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
        memcpy(&shadowPC[16], srcWorld, sizeof(float) * 16);

        DirectX12UploadRing::Allocation alloc;
        if (!m_uploadRing.Allocate(sizeof(shadowPC), alloc)) return;
        memcpy(alloc.cpu, shadowPC, sizeof(shadowPC));
        commandList->SetGraphicsRootConstantBufferView(slot, alloc.gpu);
        return;
    }

    // Constant contents are copied into this frame's ring slice on the
    // first bind after an update, so frames in flight keep their own copy.
    if (const void* data = dx12Buf->GetConstantData()) {
        D3D12_GPU_VIRTUAL_ADDRESS address =
            dx12Buf->GetFrameAddress(m_frameSerial);
        if (!address) {
            const size_t size = dx12Buf->GetConstantDataSize();
            DirectX12UploadRing::Allocation alloc;
            if (m_uploadRing.Allocate(size, alloc)) {
                memcpy(alloc.cpu, data, size);
                address = alloc.gpu;
                dx12Buf->SetFrameAddress(m_frameSerial, address);
            }
        }
        if (address) {
            commandList->SetGraphicsRootConstantBufferView(slot, address);
            return;
        }
    }

    if (!dx12Buf->GetD3DBuffer()) return;
    commandList->SetGraphicsRootConstantBufferView(
        slot, dx12Buf->GetD3DBuffer()->GetGPUVirtualAddress());
}

BufferBase* DirectX12Renderer::CreateBuffer(BufferType Type, uint32_t size,
                                             void* data) {
    assert(size > 0);
    auto* buffer = new DirectX12Buffer(device.Get(), commandQueue.Get(), size,
                                       Type, &m_uploader);
    if (!buffer->Initialize(data)) {
        delete buffer;
        return nullptr;
    }
    return buffer;
}

Shader* DirectX12Renderer::CreateShader(const std::string& shaderSource) {
    auto* shader = new DirectX12Shader(device.Get());
    if (!shader->compile(shaderSource)) {
        delete shader;
        return nullptr;
    }

    ID3DBlob* vs = shader->getVertexShaderBlob();
    ID3DBlob* ps = shader->getPixelShaderBlob();
    PipelineKey key;
    key.vsHash = HashBytecode(vs);
    key.psHash = HashBytecode(ps);
    if (ID3D12PipelineState* pso = GetPipeline(key, vs, ps)) {
        shader->SetPipelineState(
            Microsoft::WRL::ComPtr<ID3D12PipelineState>(pso));
        if (!m_defaultPipelineState) {
            m_defaultPipelineState = pso;
            m_cachedVSBlob = vs;
        }
    }
    shader->SetCommandList(commandList.Get());
    shader->SetRenderer(this);
    return shader;
}

Texture* DirectX12Renderer::CreateTexture(
    const std::string& TexturePath) {
    auto* texture = new DirectX12Texture(device.Get(), commandQueue.Get(),
                                         commandList.Get(), &m_uploader);
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
    auto* texture = new DirectX12Texture(device.Get(), commandQueue.Get(),
                                         commandList.Get(), &m_uploader);
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
    auto* texture = new DirectX12CubemapTexture(
        device.Get(), commandQueue.Get(), &m_uploader);
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
    auto* texture = new DirectX12CubemapTexture(
        device.Get(), commandQueue.Get(), &m_uploader);
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

void DirectX12Renderer::BindLightConstants() {
    DirectX12UploadRing::Allocation alloc;
    if (!m_uploadRing.Allocate(m_lightData.size(), alloc)) return;
    memcpy(alloc.cpu, m_lightData.data(), m_lightData.size());
    commandList->SetGraphicsRootConstantBufferView(3, alloc.gpu);
}

void DirectX12Renderer::UpdateShadowLightUBO(const void* data, uint32_t size) {
    if (!data) return;

    size_t copySize = size;
    if (copySize > m_lightData.size()) copySize = m_lightData.size();
    memcpy(m_lightData.data(), data, copySize);

    // The ring slice bound at BeginRender holds last frame's values
    if (m_frameActive) BindLightConstants();
}

void DirectX12Renderer::SetLightVP(const float* mat) {
    if (mat) {
        memcpy(m_pendingLightVP, mat, sizeof(m_pendingLightVP));
        m_hasPendingLightVP = true;
    }
}

bool DirectX12Renderer::CreateShadowMapResources() {
    if (m_shadowMapCreated) return true;

    // Create shadow DSV heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_shadowDsvHeap)))) {
        SLEAK_ERROR("Failed to create shadow DSV heap!");
        return false;
    }
    m_shadowDsvHandle = m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();

    // Create shadow depth texture
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_shadowMapResolution;
    depthDesc.Height = m_shadowMapResolution;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
            IID_PPV_ARGS(&m_shadowDepthBuffer)))) {
        SLEAK_ERROR("Failed to create shadow depth buffer!");
        return false;
    }

    // Create DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_shadowDepthBuffer.Get(), &dsvDesc, m_shadowDsvHandle);

    // Create SRV in shared heap
    m_shadowSrvIndex = AllocateSRVSlot();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_shadowDepthBuffer.Get(), &srvDesc,
                                      GetSharedSrvCPUHandle(m_shadowSrvIndex));

    m_shadowMapCreated = true;
    m_shadowResourcesCreated = true;
    SLEAK_INFO("D3D12 shadow map resources created ({}x{})", m_shadowMapResolution, m_shadowMapResolution);
    return true;
}

void DirectX12Renderer::RenderShadowPass() {
    // Skip if no cached draws — preserve previous frame's shadow map
    auto* queue = RenderCommandQueue::GetInstance();
    if (!queue || !queue->HasCachedShadowDraws()) return;

    if (!m_shadowMapCreated) {
        if (!CreateShadowMapResources()) return;
    }
    if (!CreateShadowPassPSO()) return;

    // Transition shadow buffer to depth write
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_shadowDepthBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    // Set shadow DSV (no RTV)
    commandList->OMSetRenderTargets(0, nullptr, FALSE, &m_shadowDsvHandle);
    commandList->ClearDepthStencilView(m_shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Set shadow viewport
    D3D12_VIEWPORT shadowViewport = {};
    shadowViewport.Width = static_cast<float>(m_shadowMapResolution);
    shadowViewport.Height = static_cast<float>(m_shadowMapResolution);
    shadowViewport.MinDepth = 0.0f;
    shadowViewport.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &shadowViewport);

    D3D12_RECT shadowScissor = {};
    shadowScissor.right = m_shadowMapResolution;
    shadowScissor.bottom = m_shadowMapResolution;
    commandList->RSSetScissorRects(1, &shadowScissor);

    // Depth-only shadow PSO (no PS, no RTV, CULL_NONE + depth bias)
    commandList->SetPipelineState(m_shadowPassPSO);
    commandList->SetGraphicsRootSignature(rootSignature.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Re-bind SRV heap
    ID3D12DescriptorHeap* srvHeaps[] = {m_sharedSrvHeap.Get()};
    commandList->SetDescriptorHeaps(1, srvHeaps);

    // Bind default texture so draws with texture lookups don't crash
    if (m_defaultTexture) {
        m_defaultTexture->Bind(0);
    }
    BindLightConstants();

    // Execute shadow draw commands
    m_inShadowPass = true;
    if (queue) {
        queue->ExecuteShadowPass(this);
    }
    m_inShadowPass = false;

    // Transition back to shader resource
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);
}

bool DirectX12Renderer::CreateSkyboxPipelineState() {
    if (m_skyboxPipelineState) return true;
    if (m_skyboxPipelineFailed) return false;
    m_skyboxPipelineFailed = true;

    auto vs =
        CompileStage(L"assets/shaders/skybox_dx12.hlsl", "VS_Main", "vs_5_0");
    auto ps =
        CompileStage(L"assets/shaders/skybox_dx12.hlsl", "PS_Main", "ps_5_0");
    if (!vs || !ps) return false;

    // No culling, depth write off with LEQUAL, and depth clip off so the
    // .xyww trick at z=1.0 is not clipped
    PipelineKey key;
    key.vsHash = HashBytecode(vs.Get());
    key.psHash = HashBytecode(ps.Get());
    key.cullMode = D3D12_CULL_MODE_NONE;
    key.depthClip = FALSE;
    key.depthWrite = D3D12_DEPTH_WRITE_MASK_ZERO;
    key.depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    m_skyboxPipelineState = GetPipeline(key, vs.Get(), ps.Get());
    if (!m_skyboxPipelineState) return false;

    m_skyboxPipelineFailed = false;
    SLEAK_INFO("DirectX 12 skybox pipeline state created successfully");
    return true;
}

void DirectX12Renderer::BindShaderPipeline(ID3D12PipelineState* pso) {
    if (!pso || !commandList) return;
    m_shaderPipeline = pso;
    if (m_passPipeline) return;
    commandList->SetPipelineState(pso);
}

void DirectX12Renderer::RestoreShaderPipeline() {
    ID3D12PipelineState* pso =
        m_shaderPipeline ? m_shaderPipeline : m_defaultPipelineState;
    if (pso) commandList->SetPipelineState(pso);
}

void DirectX12Renderer::BeginSkyboxPass() {
    if (!CreateSkyboxPipelineState()) {
        SLEAK_WARN("BeginSkyboxPass: failed to create skybox PSO");
        return;
    }
    m_passPipeline = m_skyboxPipelineState;
    commandList->SetPipelineState(m_skyboxPipelineState);
}

void DirectX12Renderer::EndSkyboxPass() {
    m_passPipeline = nullptr;
    RestoreShaderPipeline();
}

bool DirectX12Renderer::CreateDebugLinePipelineState() {
    if (m_debugLinePipelineState) return true;
    if (m_debugLinePipelineFailed) return false;
    m_debugLinePipelineFailed = true;

    auto vs = CompileStage(L"assets/shaders/debug_line_dx12.hlsl", "VS_Main",
                           "vs_5_0");
    auto ps = CompileStage(L"assets/shaders/debug_line_dx12.hlsl", "PS_Main",
                           "ps_5_0");
    if (!vs || !ps) return false;

    PipelineKey key;
    key.vsHash = HashBytecode(vs.Get());
    key.psHash = HashBytecode(ps.Get());
    key.cullMode = D3D12_CULL_MODE_NONE;
    key.depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    key.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;

    m_debugLinePipelineState = GetPipeline(key, vs.Get(), ps.Get());
    if (!m_debugLinePipelineState) return false;

    m_debugLinePipelineFailed = false;
    SLEAK_INFO("DirectX 12 debug line pipeline state created successfully");
    return true;
}

void DirectX12Renderer::BeginDebugLinePass() {
    if (!CreateDebugLinePipelineState()) {
        SLEAK_WARN("BeginDebugLinePass: failed to create debug line PSO");
        return;
    }
    m_passPipeline = m_debugLinePipelineState;
    commandList->SetPipelineState(m_debugLinePipelineState);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
}

void DirectX12Renderer::EndDebugLinePass() {
    m_passPipeline = nullptr;
    RestoreShaderPipeline();
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

    ImGui_ImplDX12_InitInfo imguiInit;
    imguiInit.Device = device.Get();
    imguiInit.CommandQueue = commandQueue.Get();
    imguiInit.NumFramesInFlight = FrameCount;
    imguiInit.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    imguiInit.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    imguiInit.SrvDescriptorHeap = m_sharedSrvHeap.Get();
    imguiInit.LegacySingleSrvCpuDescriptor = imguiCpuHandle;
    imguiInit.LegacySingleSrvGpuDescriptor = imguiGpuHandle;

    if (!ImGui_ImplDX12_Init(&imguiInit)) return false;

    bImInitialized = true;
    return true;
}

}  // namespace RenderEngine
}  // namespace Sleak

#endif
