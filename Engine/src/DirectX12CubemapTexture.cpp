#include "../../include/private/Graphics/DirectX/DirectX12CubemapTexture.hpp"

#ifdef PLATFORM_WIN

#include <Logger.hpp>
#include <stb_image.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace Sleak {
namespace RenderEngine {

DirectX12CubemapTexture::DirectX12CubemapTexture(
    ID3D12Device* device, ID3D12CommandQueue* commandQueue)
    : m_device(device), m_commandQueue(commandQueue) {}

DirectX12CubemapTexture::~DirectX12CubemapTexture() {
    m_uploadBuffer.Reset();
    m_srvHeap.Reset();
    m_texture.Reset();
}

bool DirectX12CubemapTexture::LoadCubemap(
    const std::array<std::string, 6>& facePaths) {
    stbi_set_flip_vertically_on_load(false);

    struct FaceData {
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
    };
    std::array<FaceData, 6> faces;

    for (int i = 0; i < 6; i++) {
        int channels;
        faces[i].pixels = stbi_load(facePaths[i].c_str(), &faces[i].w,
                                     &faces[i].h, &channels, 4);
        if (!faces[i].pixels) {
            SLEAK_ERROR(
                "DirectX12CubemapTexture: Failed to load face {}: {}", i,
                facePaths[i]);
            for (int j = 0; j < i; j++)
                stbi_image_free(faces[j].pixels);
            return false;
        }
    }

    for (int i = 1; i < 6; i++) {
        if (faces[i].w != faces[0].w || faces[i].h != faces[0].h) {
            SLEAK_ERROR(
                "DirectX12CubemapTexture: Face {} size ({}x{}) doesn't match "
                "face 0 ({}x{})",
                i, faces[i].w, faces[i].h, faces[0].w, faces[0].h);
            for (int j = 0; j < 6; j++)
                stbi_image_free(faces[j].pixels);
            return false;
        }
    }

    uint32_t faceSize = static_cast<uint32_t>(faces[0].w);
    std::vector<unsigned char*> facePointers;
    for (int i = 0; i < 6; i++)
        facePointers.push_back(faces[i].pixels);

    bool result = CreateCubemapFromFaces(facePointers, faceSize);

    for (int i = 0; i < 6; i++)
        stbi_image_free(faces[i].pixels);

    if (result) {
        SLEAK_INFO(
            "DirectX12CubemapTexture: Loaded cubemap ({}x{}, 6 faces)",
            faceSize, faceSize);
    }
    return result;
}

bool DirectX12CubemapTexture::LoadEquirectangular(const std::string& path,
                                                    uint32_t faceSize) {
    stbi_set_flip_vertically_on_load(false);

    int panW, panH, channels;
    unsigned char* panorama =
        stbi_load(path.c_str(), &panW, &panH, &channels, 4);
    if (!panorama) {
        SLEAK_ERROR(
            "DirectX12CubemapTexture: Failed to load panorama: {}", path);
        return false;
    }

    size_t faceBytes = static_cast<size_t>(faceSize) * faceSize * 4;
    std::vector<std::vector<unsigned char>> allFaces(6);
    for (int i = 0; i < 6; i++)
        allFaces[i].resize(faceBytes);

    for (int face = 0; face < 6; ++face) {
        unsigned char* faceData = allFaces[face].data();

        for (uint32_t y = 0; y < faceSize; ++y) {
            for (uint32_t x = 0; x < faceSize; ++x) {
                float u = (2.0f * (x + 0.5f) / faceSize) - 1.0f;
                float v = (2.0f * (y + 0.5f) / faceSize) - 1.0f;

                float dx, dy, dz;
                switch (face) {
                    case 0: dx =  1.0f; dy = -v;    dz = -u;    break; // +X
                    case 1: dx = -1.0f; dy = -v;    dz =  u;    break; // -X
                    case 2: dx =  u;    dy =  1.0f; dz =  v;    break; // +Y
                    case 3: dx =  u;    dy = -1.0f; dz = -v;    break; // -Y
                    case 4: dx =  u;    dy = -v;    dz =  1.0f; break; // +Z
                    case 5: dx = -u;    dy = -v;    dz = -1.0f; break; // -Z
                    default: dx = dy = dz = 0.0f; break;
                }

                float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= len; dy /= len; dz /= len;

                float lon = std::atan2(dz, dx);
                float lat = std::asin(std::clamp(dy, -1.0f, 1.0f));

                float panU = 0.5f + lon / (2.0f * 3.14159265f);
                float panV = 0.5f - lat / 3.14159265f;

                float srcX = panU * (panW - 1);
                float srcY = panV * (panH - 1);
                int x0 = static_cast<int>(srcX);
                int y0 = static_cast<int>(srcY);
                int x1 = std::min(x0 + 1, panW - 1);
                int y1 = std::min(y0 + 1, panH - 1);
                x0 = std::clamp(x0, 0, panW - 1);
                y0 = std::clamp(y0, 0, panH - 1);
                float fx = srcX - x0;
                float fy = srcY - y0;

                size_t idx = (y * faceSize + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    float c00 = panorama[(y0 * panW + x0) * 4 + c];
                    float c10 = panorama[(y0 * panW + x1) * 4 + c];
                    float c01 = panorama[(y1 * panW + x0) * 4 + c];
                    float c11 = panorama[(y1 * panW + x1) * 4 + c];
                    float val = c00 * (1 - fx) * (1 - fy) +
                                c10 * fx * (1 - fy) +
                                c01 * (1 - fx) * fy +
                                c11 * fx * fy;
                    faceData[idx + c] = static_cast<unsigned char>(
                        std::clamp(val, 0.0f, 255.0f));
                }
            }
        }
    }

    stbi_image_free(panorama);

    std::vector<unsigned char*> facePointers;
    for (int i = 0; i < 6; i++)
        facePointers.push_back(allFaces[i].data());

    bool result = CreateCubemapFromFaces(facePointers, faceSize);

    if (result) {
        SLEAK_INFO(
            "DirectX12CubemapTexture: Loaded equirectangular panorama "
            "({}x{} face)",
            faceSize, faceSize);
    }
    return result;
}

bool DirectX12CubemapTexture::CreateCubemapFromFaces(
    const std::vector<unsigned char*>& faceData, uint32_t faceSize) {
    if (!m_device || faceData.size() != 6) return false;

    m_width = faceSize;
    m_height = faceSize;

    // Create cubemap texture resource (6 array slices)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = faceSize;
    texDesc.Height = faceSize;
    texDesc.DepthOrArraySize = 6;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heapProps = {};
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
            "DirectX12CubemapTexture: Failed to create texture resource "
            "HRESULT: 0x{:08X}",
            static_cast<unsigned int>(hr));
        return false;
    }

    // Get copyable footprints for all 6 subresources
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[6];
    UINT numRows[6];
    UINT64 rowSizeInBytes[6];
    UINT64 totalBytes;
    m_device->GetCopyableFootprints(&texDesc, 0, 6, 0, footprints, numRows,
                                     rowSizeInBytes, &totalBytes);

    // Create upload buffer
    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeapProps.CreationNodeMask = 1;
    uploadHeapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC uploadDesc = {};
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

    hr = m_device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_uploadBuffer));

    if (FAILED(hr)) {
        SLEAK_ERROR(
            "DirectX12CubemapTexture: Failed to create upload buffer");
        return false;
    }

    // Map and copy data for all 6 faces
    BYTE* mapped = nullptr;
    hr = m_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) {
        SLEAK_ERROR("DirectX12CubemapTexture: Failed to map upload buffer");
        return false;
    }

    uint32_t srcRowPitch = faceSize * 4;
    for (int face = 0; face < 6; face++) {
        BYTE* destBase = mapped + footprints[face].Offset;
        const BYTE* srcData = reinterpret_cast<const BYTE*>(faceData[face]);

        for (UINT row = 0; row < numRows[face]; row++) {
            memcpy(destBase + row * footprints[face].Footprint.RowPitch,
                   srcData + row * srcRowPitch, srcRowPitch);
        }
    }

    m_uploadBuffer->Unmap(0, nullptr);

    // Create command allocator and list for the upload
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmdAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;

    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&cmdAllocator));
    if (FAILED(hr)) return false;

    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      cmdAllocator.Get(), nullptr,
                                      IID_PPV_ARGS(&cmdList));
    if (FAILED(hr)) return false;

    // Copy each face from upload buffer to texture
    for (UINT face = 0; face < 6; face++) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = m_texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = face;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = m_uploadBuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[face];

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Transition to PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barrier = {};
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

    ID3D12CommandList* ppCmdLists[] = {cmdList.Get()};
    m_commandQueue->ExecuteCommandLists(1, ppCmdLists);
    WaitForUpload();

    // Create SRV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    hr = m_device->CreateDescriptorHeap(&srvHeapDesc,
                                         IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        SLEAK_ERROR(
            "DirectX12CubemapTexture: Failed to create SRV descriptor heap");
        return false;
    }

    // Create SRV for TextureCube
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    m_device->CreateShaderResourceView(
        m_texture.Get(), &srvDesc,
        m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

void DirectX12CubemapTexture::WaitForUpload() {
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

bool DirectX12CubemapTexture::LoadFromMemory(const void* data, uint32_t width,
                                              uint32_t height,
                                              TextureFormat format) {
    (void)data;
    (void)width;
    (void)height;
    (void)format;
    return false;
}

bool DirectX12CubemapTexture::LoadFromFile(const std::string& filePath) {
    (void)filePath;
    return false;
}

void DirectX12CubemapTexture::Bind(uint32_t slot) const {
    (void)slot;
    // Binding in DX12 requires a command list — use BindToCommandList instead
}

void DirectX12CubemapTexture::Unbind() const {
    // No-op in DX12
}

void DirectX12CubemapTexture::SetFilter(TextureFilter filter) {
    (void)filter;
}

void DirectX12CubemapTexture::SetWrapMode(TextureWrapMode wrapMode) {
    (void)wrapMode;
}

void DirectX12CubemapTexture::BindToCommandList(
    ID3D12GraphicsCommandList* cmdList, UINT rootParameterIndex) const {
    if (!m_srvHeap || !cmdList) return;

    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootDescriptorTable(
        rootParameterIndex,
        m_srvHeap->GetGPUDescriptorHandleForHeapStart());
}

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // PLATFORM_WIN
