#include "../../include/private/Graphics/DirectX11/DirectX11Shader.hpp"

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <Core/Logger.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef SLEAK_HOT_RELOAD
#include <filesystem>
#endif

namespace Sleak {
namespace RenderEngine {

namespace {

constexpr const char* VERTEX_ENTRY = "VS_Main";
constexpr const char* PIXEL_ENTRY = "PS_Main";
constexpr const char* SPLIT_FILE_ENTRY = "Main";
constexpr const char* VERTEX_PROFILE = "vs_5_0";
constexpr const char* PIXEL_PROFILE = "ps_5_0";

#ifdef SLEAK_HOT_RELOAD
/// Returns the file name up to its first '.', e.g. skybox for skybox.hlsl.
std::string StemFromPath(const std::string& path) {
    const std::string name = std::filesystem::path(path).filename().string();
    return name.substr(0, name.find('.'));
}

/// Returns the live shader registry, leaked to survive static teardown.
std::unordered_set<DirectX11Shader*>& LiveShaders() {
    static auto* shaders = new std::unordered_set<DirectX11Shader*>();
    return *shaders;
}
#endif

}  // namespace

DirectX11Shader::DirectX11Shader(ID3D11Device* device) : m_device(device) {
    assert(m_device != nullptr);  // Ensure the device is valid
    m_device->GetImmediateContext(m_deviceContext.GetAddressOf());
#ifdef SLEAK_HOT_RELOAD
    LiveShaders().insert(this);
#endif
}

DirectX11Shader::~DirectX11Shader() {
#ifdef SLEAK_HOT_RELOAD
    LiveShaders().erase(this);
#endif
}

#ifdef SLEAK_HOT_RELOAD
const std::unordered_set<DirectX11Shader*>& DirectX11Shader::GetLiveShaders() {
    return LiveShaders();
}

bool DirectX11Shader::Reload() {
    if (m_sources.vertPath.empty() || m_sources.fragPath.empty()) return false;
    return BuildStages(m_sources);
}

void DirectX11Shader::AdoptStages(const DirectX11Shader& source) {
    m_vertexShader = source.m_vertexShader;
    m_pixelShader = source.m_pixelShader;
    m_vertexShaderBlob = source.m_vertexShaderBlob;
    m_pixelShaderBlob = source.m_pixelShaderBlob;
    Layout = source.Layout;
}
#endif

bool DirectX11Shader::compile(const std::string& shaderPath) {
    return BuildStages({shaderPath, VERTEX_ENTRY, shaderPath, PIXEL_ENTRY});
}

bool DirectX11Shader::compile(const std::string& vertPath,
                              const std::string& fragPath) {
    return BuildStages(
        {vertPath, SPLIT_FILE_ENTRY, fragPath, SPLIT_FILE_ENTRY});
}

bool DirectX11Shader::BuildStages(StageSources sources) {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;
    if (!compileShader(sources.vertPath, sources.vertEntry, VERTEX_PROFILE,
                       vertexShader, vertexBlob)) {
        SLEAK_ERROR("Failed to compile vertex shader {}", sources.vertPath);
        return false;
    }
    if (!compileShader(sources.fragPath, sources.fragEntry, PIXEL_PROFILE,
                       pixelShader, pixelBlob)) {
        SLEAK_ERROR("Failed to compile pixel shader {}", sources.fragPath);
        return false;
    }

    // A rebuild needs a layout validated against the new vertex inputs
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    if (Layout && !CreateLayoutFor(vertexBlob.Get(), layout)) return false;

    m_vertexShader = std::move(vertexShader);
    m_pixelShader = std::move(pixelShader);
    m_vertexShaderBlob = std::move(vertexBlob);
    m_pixelShaderBlob = std::move(pixelBlob);
    if (layout) Layout = std::move(layout);
#ifdef SLEAK_HOT_RELOAD
    m_vertStem = StemFromPath(sources.vertPath);
    m_fragStem = StemFromPath(sources.fragPath);
    m_sources = std::move(sources);
#endif
    return true;
}

void DirectX11Shader::bind() {
    if (!m_vertexShader || !m_pixelShader) {
        throw std::runtime_error("Shaders are not compiled!");
    }

    m_deviceContext->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    if (Layout)
        m_deviceContext->IASetInputLayout(Layout.Get());
}

ID3DBlob* DirectX11Shader::getVertexShaderBlob() const {
    return m_vertexShaderBlob.Get();
}

ID3D11InputLayout* DirectX11Shader::createInputLayout() {
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    if (!m_vertexShaderBlob ||
        !CreateLayoutFor(m_vertexShaderBlob.Get(), layout))
        return nullptr;

    // Store per-shader so bind() can set it, and keep a ref for the caller
    Layout = layout;
    return layout.Detach();
}

bool DirectX11Shader::CreateLayoutFor(
    ID3DBlob* vertexBlob, Microsoft::WRL::ComPtr<ID3D11InputLayout>& layout) {
    // Check via reflection if the shader uses BLENDINDICES (skinned shader)
    bool isSkinned = false;
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflector;
    HRESULT hr =
        D3DReflect(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                   IID_ID3D11ShaderReflection,
                   reinterpret_cast<void**>(reflector.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_DESC shaderDesc;
        reflector->GetDesc(&shaderDesc);
        for (UINT p = 0; p < shaderDesc.InputParameters; ++p) {
            D3D11_SIGNATURE_PARAMETER_DESC paramDesc;
            reflector->GetInputParameterDesc(p, &paramDesc);
            if (strcmp(paramDesc.SemanticName, "BLENDINDICES") == 0) {
                isSkinned = true;
                break;
            }
        }
    }

    D3D11_INPUT_ELEMENT_DESC* layoutDesc = isSkinned ? SkinnedLayout : DefaultLayout;
    UINT layoutCount = isSkinned ? ARRAYSIZE(SkinnedLayout) : ARRAYSIZE(DefaultLayout);

    hr = m_device->CreateInputLayout(
        layoutDesc, layoutCount, vertexBlob->GetBufferPointer(),
        vertexBlob->GetBufferSize(), layout.ReleaseAndGetAddressOf());

    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create input layout: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return false;
    }
    return true;
}

/// Compiles a shader stage from an HLSL file and creates the matching D3D11 shader object.
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
            SLEAK_ERROR("Shader compilation error in {} ({}): {}", filePath,
                        entryPoint, (char*)errorBlob->GetBufferPointer());
        } else {
            SLEAK_ERROR("Shader compilation error in {} ({}): 0x{:08X}",
                        filePath, entryPoint, static_cast<unsigned int>(hr));
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

    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create shader object for {} ({}): 0x{:08X}",
                    filePath, entryPoint, static_cast<unsigned int>(hr));
    }
    return SUCCEEDED(hr);
}

}  // namespace RenderEngine
}  // namespace Sleak