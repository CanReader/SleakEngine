#ifndef DIRECTX12CUBEMAPTEXTURE_HPP_
#define DIRECTX12CUBEMAPTEXTURE_HPP_

#include <Core/OSDef.hpp>

#ifdef PLATFORM_WIN

#include <Runtime/Texture.hpp>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <string>
#include <vector>

namespace Sleak {
namespace RenderEngine {

/// D3D12 cubemap texture, loadable from six face images or a single equirectangular panorama.
class ENGINE_API DirectX12CubemapTexture : public ::Sleak::Texture {
public:
    DirectX12CubemapTexture(ID3D12Device* device,
                            ID3D12CommandQueue* commandQueue);
    ~DirectX12CubemapTexture() override;

    /// Loads and uploads 6 face images: +X, -X, +Y, -Y, +Z, -Z.
    bool LoadCubemap(const std::array<std::string, 6>& facePaths);
    /// Loads a single equirectangular panorama and resamples it into 6 cube faces.
    bool LoadEquirectangular(const std::string& path, uint32_t faceSize = 512);

    // Texture interface
    /// Unused for cubemaps; load via LoadCubemap/LoadEquirectangular instead.
    bool LoadFromMemory(const void* data, uint32_t width, uint32_t height,
                        TextureFormat format) override;
    /// Unused for cubemaps; load via LoadCubemap/LoadEquirectangular instead.
    bool LoadFromFile(const std::string& filePath) override;

    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;

    void SetFilter(TextureFilter filter) override;
    void SetWrapMode(TextureWrapMode wrapMode) override;

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    TextureFormat GetFormat() const override { return TextureFormat::RGBA8; }
    TextureType GetType() const override { return TextureType::TextureCube; }

    ID3D12Resource* GetResource() const { return m_texture.Get(); }

    /// Binds this cubemap's SRV table at the given root parameter for a draw.
    void BindToCommandList(ID3D12GraphicsCommandList* cmdList,
                           UINT rootParameterIndex) const;

    // Set shared SRV heap slot (called by renderer during creation)
    void SetSharedSrvGPUHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) { m_srvGpuHandle = handle; m_usesSharedHeap = true; }
    // Create SRV directly into an externally-provided CPU handle (cube SRV)
    void CreateSRVIntoHandle(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);

private:
    /// Builds the cubemap texture array and shader resource view from decoded face pixels.
    bool CreateCubemapFromFaces(const std::vector<unsigned char*>& faceData,
                                uint32_t faceSize);
    /// Blocks until the upload-heap copy to the GPU-resident cubemap resource completes.
    void WaitForUpload();

    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_commandQueue = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap; // fallback
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpuHandle = {};
    bool m_usesSharedHeap = false;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // PLATFORM_WIN

#endif  // DIRECTX12CUBEMAPTEXTURE_HPP_
