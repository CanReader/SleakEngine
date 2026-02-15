#include "../../include/private/Graphics/DirectX/DirectX11CubemapTexture.hpp"

#ifdef PLATFORM_WIN

#include <Logger.hpp>
#include <stb_image.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace Sleak {
namespace RenderEngine {

DirectX11CubemapTexture::DirectX11CubemapTexture(ID3D11Device* device)
    : m_device(device) {
    device->GetImmediateContext(m_deviceContext.GetAddressOf());
}

DirectX11CubemapTexture::~DirectX11CubemapTexture() {
    m_shaderResourceView.Reset();
    m_texture.Reset();
    m_samplerState.Reset();
}

bool DirectX11CubemapTexture::LoadCubemap(
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
            SLEAK_ERROR("DirectX11CubemapTexture: Failed to load face {}: {}",
                        i, facePaths[i]);
            for (int j = 0; j < i; j++)
                stbi_image_free(faces[j].pixels);
            return false;
        }
    }

    // All faces must be same size
    for (int i = 1; i < 6; i++) {
        if (faces[i].w != faces[0].w || faces[i].h != faces[0].h) {
            SLEAK_ERROR(
                "DirectX11CubemapTexture: Face {} size ({}x{}) doesn't match "
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
        SLEAK_INFO("DirectX11CubemapTexture: Loaded cubemap ({}x{}, 6 faces)",
                   faceSize, faceSize);
    }
    return result;
}

bool DirectX11CubemapTexture::LoadEquirectangular(const std::string& path,
                                                    uint32_t faceSize) {
    stbi_set_flip_vertically_on_load(false);

    int panW, panH, channels;
    unsigned char* panorama =
        stbi_load(path.c_str(), &panW, &panH, &channels, 4);
    if (!panorama) {
        SLEAK_ERROR("DirectX11CubemapTexture: Failed to load panorama: {}",
                    path);
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
            "DirectX11CubemapTexture: Loaded equirectangular panorama "
            "({}x{} face)",
            faceSize, faceSize);
    }
    return result;
}

bool DirectX11CubemapTexture::CreateCubemapFromFaces(
    const std::vector<unsigned char*>& faceData, uint32_t faceSize) {
    m_width = faceSize;
    m_height = faceSize;

    // Create texture description for cubemap
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = faceSize;
    texDesc.Height = faceSize;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 6;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    // Prepare subresource data for all 6 faces
    D3D11_SUBRESOURCE_DATA subData[6] = {};
    uint32_t rowPitch = faceSize * 4;
    for (int i = 0; i < 6; i++) {
        subData[i].pSysMem = faceData[i];
        subData[i].SysMemPitch = rowPitch;
        subData[i].SysMemSlicePitch = 0;
    }

    HRESULT hr =
        m_device->CreateTexture2D(&texDesc, subData, m_texture.GetAddressOf());
    if (FAILED(hr)) {
        SLEAK_ERROR(
            "DirectX11CubemapTexture: Failed to create cubemap texture! "
            "HRESULT: 0x{:08X}",
            static_cast<unsigned int>(hr));
        return false;
    }

    // Create SRV for cubemap
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(m_texture.Get(), &srvDesc,
                                             m_shaderResourceView.GetAddressOf());
    if (FAILED(hr)) {
        SLEAK_ERROR(
            "DirectX11CubemapTexture: Failed to create SRV! HRESULT: "
            "0x{:08X}",
            static_cast<unsigned int>(hr));
        return false;
    }

    CreateSamplerState();
    return true;
}

void DirectX11CubemapTexture::CreateSamplerState() {
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr =
        m_device->CreateSamplerState(&samplerDesc, m_samplerState.GetAddressOf());
    if (FAILED(hr)) {
        SLEAK_ERROR(
            "DirectX11CubemapTexture: Failed to create sampler state! "
            "HRESULT: 0x{:08X}",
            static_cast<unsigned int>(hr));
    }
}

bool DirectX11CubemapTexture::LoadFromMemory(const void* data, uint32_t width,
                                              uint32_t height,
                                              TextureFormat format) {
    (void)data;
    (void)width;
    (void)height;
    (void)format;
    return false;
}

bool DirectX11CubemapTexture::LoadFromFile(const std::string& filePath) {
    (void)filePath;
    return false;
}

void DirectX11CubemapTexture::Bind(uint32_t slot) const {
    if (m_shaderResourceView && m_samplerState) {
        m_deviceContext->PSSetShaderResources(
            slot, 1, m_shaderResourceView.GetAddressOf());
        m_deviceContext->PSSetSamplers(slot, 1, m_samplerState.GetAddressOf());
    }
}

void DirectX11CubemapTexture::Unbind() const {
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11SamplerState* nullSampler = nullptr;
    m_deviceContext->PSSetShaderResources(0, 1, &nullSRV);
    m_deviceContext->PSSetSamplers(0, 1, &nullSampler);
}

void DirectX11CubemapTexture::SetFilter(TextureFilter filter) {
    (void)filter;
}

void DirectX11CubemapTexture::SetWrapMode(TextureWrapMode wrapMode) {
    (void)wrapMode;
}

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // PLATFORM_WIN
