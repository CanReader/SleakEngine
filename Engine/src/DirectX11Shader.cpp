#include "../../include/private/Graphics/DirectX/DirectX11Shader.hpp"

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <Logger.hpp>
#include <stdexcept>
#include <string>

namespace Sleak {
namespace RenderEngine {

DirectX11Shader::DirectX11Shader(ID3D11Device* device) : m_device(device) {
    assert(m_device != nullptr);  // Ensure the device is valid
    m_device->GetImmediateContext(m_deviceContext.GetAddressOf());
}

DirectX11Shader::~DirectX11Shader() {
    // Resources are automatically released by ComPtr
}

bool DirectX11Shader::compile(const std::string& shaderPath) {
    // Compile vertex shader
    if (!compileShader(shaderPath, "VS_Main", "vs_5_0", m_vertexShader,
                       m_vertexShaderBlob)) {
        SLEAK_ERROR("Failed to compile vertex shader!");
        return false;
    }

    // Compile pixel shader
    if (!compileShader(shaderPath, "PS_Main", "ps_5_0", m_pixelShader,
                       m_pixelShaderBlob)) {
        SLEAK_ERROR("Failed to compile pixel shader!");
        return false;
    }

    return true;
}

bool DirectX11Shader::compile(const std::string& vertPath,
                              const std::string& fragPath) {
    // Compile vertex shader
    if (!compileShader(vertPath, "Main", "vs_5_0", m_vertexShader,
                       m_vertexShaderBlob)) {
        SLEAK_ERROR("Failed to compile vertex shader!");
        return false;
    }

    // Compile pixel shader
    if (!compileShader(fragPath, "Main", "ps_5_0", m_pixelShader,
                       m_pixelShaderBlob)) {
        SLEAK_ERROR("Failed to compile pixel shader!");
        return false;
    }

    return true;
}

void DirectX11Shader::bind() {
    if (!m_vertexShader || !m_pixelShader) {
        throw std::runtime_error("Shaders are not compiled!");
    }

    m_deviceContext->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_pixelShader.Get(), nullptr, 0);
}

ID3DBlob* DirectX11Shader::getVertexShaderBlob() const {
    return m_vertexShaderBlob.Get();
}

ID3D11InputLayout* DirectX11Shader::createInputLayout() {
    ID3D11InputLayout* inputLayout = nullptr;

    HRESULT hr = m_device->CreateInputLayout(
        DefaultLayout, ARRAYSIZE(DefaultLayout),
        m_vertexShaderBlob->GetBufferPointer(),
        m_vertexShaderBlob->GetBufferSize(), &inputLayout); // Here crashes!

    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create input layout: 0x{:08X}",
                    hr);  // Log the HRESULT
        return nullptr;
    }

    return inputLayout;
}

template <typename T>
bool DirectX11Shader::compileShader(const std::string& filePath,
                                    const std::string& entryPoint,
                                    const std::string& profile,
                                    Microsoft::WRL::ComPtr<T>& shader,
                                    Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(
        std::wstring(filePath.begin(), filePath.end()).c_str(),  // File path
        nullptr,                                                 // Defines
        D3D_COMPILE_STANDARD_FILE_INCLUDE,                       // Includes
        entryPoint.c_str(),                                      // Entry point
        profile.c_str(),               // Shader profile (e.g., "vs_5_0")
        D3DCOMPILE_DEBUG,  // Flags
        0,                             // Flags 2
        &blob,                         // Output blob
        &errorBlob                     // Error blob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            SLEAK_ERROR("Shader compilation error: {}",
                        (char*)errorBlob->GetBufferPointer());
        }
        return false;
    }

    // Create the shader object
    if constexpr (std::is_same_v<T, ID3D11VertexShader>) {
        hr = m_device->CreateVertexShader(
            blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
            reinterpret_cast<ID3D11VertexShader**>(shader.GetAddressOf()));
    } else if constexpr (std::is_same_v<T, ID3D11PixelShader>) {
        hr = m_device->CreatePixelShader(
            blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
            reinterpret_cast<ID3D11PixelShader**>(shader.GetAddressOf()));
    }

    return SUCCEEDED(hr);
}

}  // namespace RenderEngine
}  // namespace Sleak