#include "../../include/private/Graphics/DirectX/DirectX12Texture.hpp"

#ifdef PLATFORM_WIN

#include <Logger.hpp>
#include <stb_image.h>
#include <cstring>

namespace Sleak {
namespace RenderEngine {

DirectX12Texture::DirectX12Texture(ID3D12Device* device,
                                   ID3D12CommandQueue* commandQueue,
                                   ID3D12GraphicsCommandList* commandList)
    : m_device(device), m_commandQueue(commandQueue),
      m_commandList(commandList) {}

DirectX12Texture::~DirectX12Texture() {
    m_uploadBuffer.Reset();
    m_srvHeap.Reset();
    m_texture.Reset();
}

bool DirectX12Texture::LoadFromMemory(const void* data, uint32_t width,
                                       uint32_t height,
                                       TextureFormat format) {
    if (!data || !m_device || width == 0 || height == 0) return false;

    m_width = width;
    m_height = height;
    m_format = format;

    DXGI_FORMAT dxgiFormat = GetDXGIFormat(format);

    if (!CreateTextureResource(width, height, dxgiFormat)) return false;
    if (!UploadTextureData(data, width, height)) return false;
    if (!CreateSRV(dxgiFormat)) return false;

    return true;
}

bool DirectX12Texture::LoadFromFile(const std::string& filePath) {
    int w, h, channels;
    unsigned char* pixels =
        stbi_load(filePath.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        SLEAK_ERROR("DirectX12Texture: Failed to load image: {}",
                    filePath);
        return false;
    }

    bool result = LoadFromMemory(pixels, static_cast<uint32_t>(w),
                                 static_cast<uint32_t>(h),
                                 TextureFormat::RGBA8);
    stbi_image_free(pixels);
    return result;
}

void DirectX12Texture::Bind(uint32_t slot) const {
    if (m_commandList && m_srvHeap) {
        BindToCommandList(m_commandList, 1);  // root param 1 = SRV table
    }
}

void DirectX12Texture::Unbind() const {
    // No-op in DX12
}

void DirectX12Texture::SetFilter(TextureFilter filter) {
    m_filter = filter;
    // Sampler is baked into the root signature as a static sampler.
    // A full implementation would recreate the PSO with a new sampler.
}

void DirectX12Texture::SetWrapMode(TextureWrapMode wrapMode) {
    m_wrapMode = wrapMode;
}

void DirectX12Texture::BindToCommandList(
    ID3D12GraphicsCommandList* cmdList, UINT rootParameterIndex) const {
    if (!m_srvHeap || !cmdList) return;

    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootDescriptorTable(
        rootParameterIndex,
        m_srvHeap->GetGPUDescriptorHandleForHeapStart());
}

bool DirectX12Texture::CreateTextureResource(uint32_t width,
                                              uint32_t height,
                                              DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_texture));

    if (FAILED(hr)) {
        SLEAK_ERROR(
            "DirectX12Texture: Failed to create texture resource "
            "HRESULT: 0x{:08X}",
            static_cast<unsigned int>(hr));
        return false;
    }

    return true;
}

bool DirectX12Texture::UploadTextureData(const void* data,
                                          uint32_t width,
                                          uint32_t height) {
    // Get the required upload buffer size (with row pitch alignment)
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows;
    UINT64 rowSizeInBytes;
    UINT64 totalBytes;
    D3D12_RESOURCE_DESC texDesc = m_texture->GetDesc();
    m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint,
                                    &numRows, &rowSizeInBytes,
                                    &totalBytes);

    // Create upload buffer
    D3D12_HEAP_PROPERTIES uploadHeapProps{};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeapProps.CreationNodeMask = 1;
    uploadHeapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Alignment = 0;
    uploadDesc.Width = totalBytes;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.SampleDesc.Quality = 0;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_uploadBuffer));

    if (FAILED(hr)) {
        SLEAK_ERROR(
            "DirectX12Texture: Failed to create upload buffer");
        return false;
    }

    // Map and copy data row-by-row (respecting row pitch alignment)
    BYTE* mapped = nullptr;
    hr = m_uploadBuffer->Map(0, nullptr,
                             reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) {
        SLEAK_ERROR("DirectX12Texture: Failed to map upload buffer");
        return false;
    }

    const BYTE* srcData = static_cast<const BYTE*>(data);
    UINT srcRowPitch = width * 4;  // RGBA8 = 4 bytes per pixel
    BYTE* destRow = mapped + footprint.Offset;

    for (UINT row = 0; row < numRows; row++) {
        memcpy(destRow + row * footprint.Footprint.RowPitch,
               srcData + row * srcRowPitch,
               srcRowPitch);
    }

    m_uploadBuffer->Unmap(0, nullptr);

    // Create command allocator and list for the upload
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmdAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;

    hr = m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocator));
    if (FAILED(hr)) return false;

    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                     cmdAllocator.Get(), nullptr,
                                     IID_PPV_ARGS(&cmdList));
    if (FAILED(hr)) return false;

    // Copy from upload buffer to texture
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = m_texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = m_uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Transition from COPY_DEST to PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    cmdList->Close();

    // Execute and wait
    ID3D12CommandList* ppCmdLists[] = {cmdList.Get()};
    m_commandQueue->ExecuteCommandLists(1, ppCmdLists);
    WaitForUpload();

    return true;
}

bool DirectX12Texture::CreateSRV(DXGI_FORMAT format) {
    // Create SRV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(
        &srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        SLEAK_ERROR(
            "DirectX12Texture: Failed to create SRV descriptor heap");
        return false;
    }

    // Create the SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    m_device->CreateShaderResourceView(
        m_texture.Get(), &srvDesc,
        m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

DXGI_FORMAT DirectX12Texture::GetDXGIFormat(TextureFormat format) const {
    switch (format) {
        case TextureFormat::RGBA8:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::BGRA8:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::RGB8:
            return DXGI_FORMAT_R8G8B8A8_UNORM;  // No 3-component format
        default:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

void DirectX12Texture::WaitForUpload() {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return;

    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!event) return;

    m_commandQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
    }

    CloseHandle(event);
}

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // PLATFORM_WIN
