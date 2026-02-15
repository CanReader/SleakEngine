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

class ENGINE_API DirectX12CubemapTexture : public ::Sleak::Texture {
public:
    DirectX12CubemapTexture(ID3D12Device* device,
                            ID3D12CommandQueue* commandQueue);
    ~DirectX12CubemapTexture() override;

    bool LoadCubemap(const std::array<std::string, 6>& facePaths);
    bool LoadEquirectangular(const std::string& path, uint32_t faceSize = 512);

    // Texture interface
    bool LoadFromMemory(const void* data, uint32_t width, uint32_t height,
                        TextureFormat format) override;
    bool LoadFromFile(const std::string& filePath) override;

    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;

    void SetFilter(TextureFilter filter) override;
    void SetWrapMode(TextureWrapMode wrapMode) override;

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    TextureFormat GetFormat() const override { return TextureFormat::RGBA8; }
    TextureType GetType() const override { return TextureType::TextureCube; }

    ID3D12DescriptorHeap* GetSRVHeap() const { return m_srvHeap.Get(); }
    ID3D12Resource* GetResource() const { return m_texture.Get(); }

    void BindToCommandList(ID3D12GraphicsCommandList* cmdList,
                           UINT rootParameterIndex) const;

private:
    bool CreateCubemapFromFaces(const std::vector<unsigned char*>& faceData,
                                uint32_t faceSize);
    void WaitForUpload();

    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_commandQueue = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // PLATFORM_WIN

#endif  // DIRECTX12CUBEMAPTEXTURE_HPP_
