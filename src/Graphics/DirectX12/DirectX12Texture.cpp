#include "../../include/private/Graphics/DirectX12/DirectX12Texture.hpp"

#ifdef PLATFORM_WIN

#include <stb_image.h>

#include <Core/Logger.hpp>
#include <Graphics/DirectX12/DirectX12Buffer.hpp>
#include <Graphics/DirectX12/DirectX12UploadContext.hpp>
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace Sleak {
namespace RenderEngine {

namespace {
uint32_t MipCount(uint32_t width, uint32_t height) {
    uint32_t count = 1;
    uint32_t size = (std::max)(width, height);
    while (size > 1) {
        size /= 2;
        ++count;
    }
    return count;
}

void DownsampleRGBA8(const uint8_t* src, uint32_t width, uint32_t height,
                     std::vector<uint8_t>& dst) {
    const uint32_t dstWidth = (std::max)(1u, width / 2);
    const uint32_t dstHeight = (std::max)(1u, height / 2);
    dst.resize(static_cast<size_t>(dstWidth) * dstHeight * 4);

    for (uint32_t y = 0; y < dstHeight; ++y) {
        const uint32_t y0 = (std::min)(y * 2, height - 1);
        const uint32_t y1 = (std::min)(y * 2 + 1, height - 1);
        for (uint32_t x = 0; x < dstWidth; ++x) {
            const uint32_t x0 = (std::min)(x * 2, width - 1);
            const uint32_t x1 = (std::min)(x * 2 + 1, width - 1);
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t sum = src[(y0 * width + x0) * 4 + c] +
                                     src[(y0 * width + x1) * 4 + c] +
                                     src[(y1 * width + x0) * 4 + c] +
                                     src[(y1 * width + x1) * 4 + c];
                dst[(static_cast<size_t>(y) * dstWidth + x) * 4 + c] =
                    static_cast<uint8_t>((sum + 2) / 4);
            }
        }
    }
}
}  // namespace

DirectX12Texture::DirectX12Texture(ID3D12Device* device,
                                   ID3D12CommandQueue* commandQueue,
                                   ID3D12GraphicsCommandList* commandList,
                                   DirectX12UploadContext* uploader)
    : m_device(device),
      m_commandQueue(commandQueue),
      m_commandList(commandList),
      m_uploader(uploader) {}

DirectX12Texture::~DirectX12Texture() {
    DirectX12Buffer::DeferCleanup(std::move(m_srvHeap));
    DirectX12Buffer::DeferCleanup(std::move(m_texture));
}

bool DirectX12Texture::LoadFromMemory(const void* data, uint32_t width,
                                       uint32_t height,
                                       TextureFormat format) {
    if (!data || !m_device || width == 0 || height == 0) return false;

    m_width = width;
    m_height = height;
    m_format = format;
    m_mipLevels = MipCount(width, height);

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
        BindToCommandList(m_commandList, 2);  // root param 2 = SRV table
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
    if (!cmdList) return;

    if (m_usesSharedHeap) {
        // Fast path: shared heap already bound by BeginRender, just set the table
        cmdList->SetGraphicsRootDescriptorTable(rootParameterIndex, m_srvGpuHandle);
    } else if (m_srvHeap) {
        // Legacy fallback: per-texture heap (should not happen in normal flow)
        ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(
            rootParameterIndex,
            m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    }
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
    texDesc.MipLevels = static_cast<UINT16>(m_mipLevels);
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
    const UINT mipCount = m_mipLevels;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
    std::vector<UINT> numRows(mipCount);
    std::vector<UINT64> rowSizes(mipCount);
    UINT64 totalBytes = 0;
    D3D12_RESOURCE_DESC texDesc = m_texture->GetDesc();
    m_device->GetCopyableFootprints(&texDesc, 0, mipCount, 0, footprints.data(),
                                    numRows.data(), rowSizes.data(),
                                    &totalBytes);

    DirectX12UploadContext local;
    DirectX12UploadContext* uploader = m_uploader;
    if (!uploader) {
        if (!local.Initialize(m_device, m_commandQueue)) return false;
        uploader = &local;
    }

    ID3D12Resource* staging = nullptr;
    BYTE* mapped =
        static_cast<BYTE*>(uploader->AllocateStaging(totalBytes, &staging));
    if (!mapped) {
        SLEAK_ERROR("DirectX12Texture: Failed to allocate staging memory");
        return false;
    }

    const uint8_t* level = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> current;
    std::vector<uint8_t> next;
    uint32_t levelWidth = width;
    uint32_t levelHeight = height;

    for (UINT mip = 0; mip < mipCount; ++mip) {
        const UINT rowBytes = levelWidth * 4;  // RGBA8 = 4 bytes per pixel
        BYTE* destRow = mapped + footprints[mip].Offset;
        for (UINT row = 0; row < numRows[mip]; ++row) {
            memcpy(destRow + row * footprints[mip].Footprint.RowPitch,
                   level + row * rowBytes, rowBytes);
        }

        if (mip + 1 < mipCount) {
            DownsampleRGBA8(level, levelWidth, levelHeight, next);
            current.swap(next);
            level = current.data();
            levelWidth = (std::max)(1u, levelWidth / 2);
            levelHeight = (std::max)(1u, levelHeight / 2);
        }
    }

    ID3D12GraphicsCommandList* cmdList = uploader->GetCommandList();
    for (UINT mip = 0; mip < mipCount; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = mip;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = staging;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[mip];

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

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

    if (uploader == &local) local.Flush();
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
    srvDesc.Texture2D.MipLevels = m_mipLevels;

    m_device->CreateShaderResourceView(
        m_texture.Get(), &srvDesc,
        m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

bool DirectX12Texture::CreateSRVIntoHandle(DXGI_FORMAT format,
                                            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = m_mipLevels;
    m_device->CreateShaderResourceView(m_texture.Get(), &srvDesc, cpuHandle);
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

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // PLATFORM_WIN
