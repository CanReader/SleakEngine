#ifndef DIRECTX12TEXTURE_HPP_
#define DIRECTX12TEXTURE_HPP_

#include <Core/OSDef.hpp>

#ifdef PLATFORM_WIN

#include <Runtime/Texture.hpp>
#include <d3d12.h>
#include <wrl/client.h>

namespace Sleak {
namespace RenderEngine {

class ENGINE_API DirectX12Texture : public Texture {
public:
    DirectX12Texture(ID3D12Device* device,
                     ID3D12CommandQueue* commandQueue,
                     ID3D12GraphicsCommandList* commandList = nullptr);
    ~DirectX12Texture() override;

    bool LoadFromMemory(const void* data, uint32_t width, uint32_t height,
                        TextureFormat format) override;
    bool LoadFromFile(const std::string& filePath) override;

    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;

    void SetFilter(TextureFilter filter) override;
    void SetWrapMode(TextureWrapMode wrapMode) override;

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    TextureFormat GetFormat() const override { return m_format; }
    TextureType GetType() const override { return TextureType::Texture2D; }

    uint64_t GetImGuiTextureID() const override { return m_srvGpuHandle.ptr; }

    ID3D12Resource* GetResource() const { return m_texture.Get(); }

    // Bind SRV table to a command list for rendering (heap already set)
    void BindToCommandList(ID3D12GraphicsCommandList* cmdList,
                           UINT rootParameterIndex) const;

    // Set shared SRV heap slot (called by renderer during creation)
    void SetSharedSrvGPUHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) { m_srvGpuHandle = handle; m_usesSharedHeap = true; }

    // Create SRV directly into an externally-provided CPU handle (shared heap)
    bool CreateSRVIntoHandle(DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);

private:
    bool CreateTextureResource(uint32_t width, uint32_t height,
                               DXGI_FORMAT format);
    bool UploadTextureData(const void* data, uint32_t width,
                           uint32_t height);
    bool CreateSRV(DXGI_FORMAT format);
    DXGI_FORMAT GetDXGIFormat(TextureFormat format) const;
    void WaitForUpload();

    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_commandQueue = nullptr;
    ID3D12GraphicsCommandList* m_commandList = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap; // fallback per-texture heap
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpuHandle = {};
    bool m_usesSharedHeap = false;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGBA8;
    TextureFilter m_filter = TextureFilter::Linear;
    TextureWrapMode m_wrapMode = TextureWrapMode::Repeat;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // PLATFORM_WIN

#endif  // DIRECTX12TEXTURE_HPP_
