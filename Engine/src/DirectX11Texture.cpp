#include "../../include/private/Graphics/DirectX/DirectX11Texture.hpp"
#include <stdexcept>
#include <Logger.hpp>
#include <wincodec.h>

namespace Sleak {
namespace RenderEngine {

DirectX11Texture::DirectX11Texture(ID3D11Device* device)
    : m_device(device), m_width(0), m_height(0),
      m_format(TextureFormat::RGBA8), m_type(TextureType::Texture2D),
      m_filter(TextureFilter::Linear), m_wrapMode(TextureWrapMode::Repeat) {
    device->GetImmediateContext(m_deviceContext.GetAddressOf());
    
}

DirectX11Texture::~DirectX11Texture() {
    // Release resources
    m_shaderResourceView.Reset();
    m_texture.Reset();
    m_samplerState.Reset();
}

bool DirectX11Texture::LoadFromMemory(const void* data, uint32_t width, uint32_t height, TextureFormat format) {
    // Store texture properties
    m_width = width;
    m_height = height;
    m_format = format;

    // Create texture description
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = GetDXGIFormat(format);
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = 0;
    textureDesc.MiscFlags = 0;

    // Create texture data
    D3D11_SUBRESOURCE_DATA textureData = {};
    textureData.pSysMem = data;
    textureData.SysMemPitch = width * 4; // Assuming 4 bytes per pixel (RGBA8)
    textureData.SysMemSlicePitch = 0;

    // Create the texture
    HRESULT hr = m_device->CreateTexture2D(&textureDesc, &textureData, &m_texture);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create texture! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
        return false;
    }

    // Create the shader resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(m_texture.Get(), &srvDesc, &m_shaderResourceView);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create shader resource view! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
        return false;
    }

    // Create the sampler state
    CreateSamplerState();

    SLEAK_INFO("Successfully created texture from memory: {}x{}", width, height);
    return true;
}

void DirectX11Texture::Bind(uint32_t slot) const {
    if (m_shaderResourceView && m_samplerState) {
        m_deviceContext->PSSetShaderResources(slot, 1, m_shaderResourceView.GetAddressOf());
        m_deviceContext->PSSetSamplers(slot, 1, m_samplerState.GetAddressOf());
    }
}

bool DirectX11Texture::LoadFromFile(const std::string& filePath) {
    // Initialize WIC
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create WIC imaging factory! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
        return false;
    }

    // Load the image file
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(
        std::wstring(filePath.begin(), filePath.end()).c_str(), // File path
        nullptr, // No preferred vendor
        GENERIC_READ, // Desired access
        WICDecodeMetadataCacheOnLoad, // Cache metadata
        &decoder);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to load texture file: {}", filePath);
        return false;
    }

    // Get the first frame (for multi-frame images like GIFs)
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to get frame from texture file: {}", filePath);
        return false;
    }

    // Convert the image format to 32-bit RGBA
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create WIC format converter! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
        return false;
    }

    hr = converter->Initialize(
        frame.Get(), // Input frame
        GUID_WICPixelFormat32bppRGBA, // Desired pixel format
        WICBitmapDitherTypeNone, // No dithering
        nullptr, // No palette
        0.0f, // Alpha threshold
        WICBitmapPaletteTypeCustom); // No palette
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to initialize WIC format converter! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
        return false;
    }

    // Get the image dimensions
    UINT width, height;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to get texture dimensions! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
        return false;
    }

    // Allocate memory for the image data
    std::vector<uint8_t> imageData(width * height * 4); // 4 bytes per pixel (RGBA)

    // Copy the image data into the buffer
    hr = converter->CopyPixels(
        nullptr, // No rectangle (entire image)
        width * 4, // Stride (bytes per row)
        static_cast<UINT>(imageData.size()), // Buffer size
        imageData.data()); // Destination buffer
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to copy texture pixels! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
        return false;
    }

    // Load the texture from memory
    return LoadFromMemory(imageData.data(), width, height, TextureFormat::RGBA8);
}

void DirectX11Texture::Unbind() const {
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11SamplerState* nullSampler = nullptr;
    m_deviceContext->PSSetShaderResources(0, 1, &nullSRV);
    m_deviceContext->PSSetSamplers(0, 1, &nullSampler);
}

void DirectX11Texture::SetFilter(TextureFilter filter) {
    m_filter = filter;
    CreateSamplerState();
}

void DirectX11Texture::SetWrapMode(TextureWrapMode wrapMode) {
    m_wrapMode = wrapMode;
    CreateSamplerState();
}

uint32_t DirectX11Texture::GetWidth() const {
    return m_width;
}

uint32_t DirectX11Texture::GetHeight() const {
    return m_height;
}

TextureFormat DirectX11Texture::GetFormat() const {
    return m_format;
}

TextureType DirectX11Texture::GetType() const {
    return m_type;
}

void DirectX11Texture::CreateSamplerState() {
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = GetD3D11Filter(m_filter);
    samplerDesc.AddressU = GetD3D11WrapMode(m_wrapMode);
    samplerDesc.AddressV = GetD3D11WrapMode(m_wrapMode);
    samplerDesc.AddressW = GetD3D11WrapMode(m_wrapMode);
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = m_device->CreateSamplerState(&samplerDesc, &m_samplerState);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create sampler state! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
    }
}

DXGI_FORMAT DirectX11Texture::GetDXGIFormat(TextureFormat format) const {
    switch (format) {
        case TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::RGB8: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case TextureFormat::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::DXT1: return DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::DXT5: return DXGI_FORMAT_BC3_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

D3D11_FILTER DirectX11Texture::GetD3D11Filter(TextureFilter filter) const {
    switch (filter) {
        case TextureFilter::Nearest: return D3D11_FILTER_MIN_MAG_MIP_POINT;
        case TextureFilter::Linear: return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        case TextureFilter::Anisotropic: return D3D11_FILTER_ANISOTROPIC;
        default: return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    }
}

D3D11_TEXTURE_ADDRESS_MODE DirectX11Texture::GetD3D11WrapMode(TextureWrapMode wrapMode) const {
    switch (wrapMode) {
        case TextureWrapMode::Repeat: return D3D11_TEXTURE_ADDRESS_WRAP;
        case TextureWrapMode::ClampToEdge: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case TextureWrapMode::ClampToBorder: return D3D11_TEXTURE_ADDRESS_BORDER;
        case TextureWrapMode::Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case TextureWrapMode::MirrorClampToEdge: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

} // namespace RenderEngine
} // namespace Sleak