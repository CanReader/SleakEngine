#ifndef DIRECTX11CUBEMAPTEXTURE_HPP_
#define DIRECTX11CUBEMAPTEXTURE_HPP_

#include <Runtime/Texture.hpp>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <string>

namespace Sleak {
namespace RenderEngine {

class DirectX11CubemapTexture : public ::Sleak::Texture {
public:
    DirectX11CubemapTexture(ID3D11Device* device);
    ~DirectX11CubemapTexture() override;

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

private:
    bool CreateCubemapFromFaces(const std::vector<unsigned char*>& faceData,
                                uint32_t faceSize);
    void CreateSamplerState();

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deviceContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplerState;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // DIRECTX11CUBEMAPTEXTURE_HPP_
