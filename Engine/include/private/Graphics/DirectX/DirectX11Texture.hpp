#ifndef _DIRECTX11_TEXTURE_H_
#define _DIRECTX11_TEXTURE_H_

#include <Runtime/Texture.hpp>
#include <d3d11.h>
#include <wrl/client.h>

namespace Sleak {
namespace RenderEngine {

class DirectX11Texture : public Sleak::Texture {
public:
    DirectX11Texture(ID3D11Device* device);
    ~DirectX11Texture();

    // Load texture from raw data
    bool LoadFromMemory(const void* data, uint32_t width, uint32_t height, TextureFormat format) override;

    bool LoadFromFile(const std::string& filePath) override;

    // Bind the texture to a specific slot
    void Bind(uint32_t slot = 0) const override;

    // Unbind the texture
    void Unbind() const override;

    // Set texture filter mode
    void SetFilter(TextureFilter filter) override;

    // Set texture wrap mode
    void SetWrapMode(TextureWrapMode wrapMode) override;

    // Get texture width
    uint32_t GetWidth() const override;

    // Get texture height
    uint32_t GetHeight() const override;

    // Get texture format
    TextureFormat GetFormat() const override;

    // Get texture type
    TextureType GetType() const override;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deviceContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplerState;
    uint32_t m_width;
    uint32_t m_height;
    TextureFormat m_format;
    TextureType m_type;
    TextureFilter m_filter;
    TextureWrapMode m_wrapMode;

    void CreateSamplerState();
    DXGI_FORMAT GetDXGIFormat(TextureFormat format) const;
    D3D11_FILTER GetD3D11Filter(TextureFilter filter) const;
    D3D11_TEXTURE_ADDRESS_MODE GetD3D11WrapMode(TextureWrapMode wrapMode) const;
};

} // namespace RenderEngine
} // namespace Sleak

#endif // _DIRECTX11_TEXTURE_H_