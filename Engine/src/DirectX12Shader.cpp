#include "../../include/private/Graphics/DirectX/DirectX12Shader.hpp"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <Logger.hpp>

namespace Sleak {
namespace RenderEngine {

DirectX12Shader::DirectX12Shader(ID3D12Device* device) : m_device(device) {
    assert(m_device != nullptr);
}

DirectX12Shader::~DirectX12Shader() = default;

bool DirectX12Shader::compile(const std::string& shaderPath) {
    // Strip .hlsl extension if present, then use _dx12.hlsl
    std::string dx12Path = shaderPath;
    auto hlslPos = dx12Path.rfind(".hlsl");
    if (hlslPos != std::string::npos) {
        dx12Path = dx12Path.substr(0, hlslPos);
    }
    dx12Path += "_dx12.hlsl";

    // Compile shaders
    if (!compileShader(dx12Path, "VS_Main", "vs_5_0", m_vertexShaderBlob)) {
        SLEAK_ERROR("Failed to compile vertex shader!");
        return false;
    }

    if (!compileShader(dx12Path, "PS_Main", "ps_5_0", m_pixelShaderBlob)) {
        SLEAK_ERROR("Failed to compile pixel shader!");
        return false;
    }

    return true;
}

bool DirectX12Shader::compile(const std::string& vertPath,
                            const std::string& fragPath) {
    // Compile shaders
    if (!compileShader(vertPath, "Main", "vs_5_0", m_vertexShaderBlob)) {
        SLEAK_ERROR("Failed to compile vertex shader!");
        return false;
    }

    if (!compileShader(fragPath, "Main", "ps_5_0", m_pixelShaderBlob)) {
        SLEAK_ERROR("Failed to compile pixel shader!");
        return false;
    }

    return true;
}

void DirectX12Shader::bind() {
    // In DX12, the PSO and root signature are set by the renderer's
    // BeginRender(). The shader is just a blob container.
}

ID3DBlob* DirectX12Shader::getVertexShaderBlob() const {
    return m_vertexShaderBlob.Get();
}

ID3DBlob* DirectX12Shader::getPixelShaderBlob() const {
    return m_pixelShaderBlob.Get();
}

bool DirectX12Shader::compileShader(const std::string& filePath,
                                  const std::string& entryPoint,
                                  const std::string& profile,
                                  Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(
        std::wstring(filePath.begin(), filePath.end()).c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint.c_str(),
        profile.c_str(),
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &blob,
        &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            SLEAK_ERROR("Shader error: {}", (char*)errorBlob->GetBufferPointer());
        }
        return false;
    }
    return true;
}

}  // namespace RenderEngine
}  // namespace Sleak