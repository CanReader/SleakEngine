#include "../../include/private/Graphics/DirectX12/DirectX12Shader.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <Core/Logger.hpp>
#include <utility>
#ifdef SLEAK_HOT_RELOAD
#include <filesystem>
#include <string_view>
#endif

#include "../../include/private/Graphics/DirectX12/DirectX12Renderer.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

constexpr const char* VERTEX_ENTRY = "VS_Main";
constexpr const char* PIXEL_ENTRY = "PS_Main";
constexpr const char* SPLIT_FILE_ENTRY = "Main";
constexpr const char* VERTEX_PROFILE = "vs_5_0";
constexpr const char* PIXEL_PROFILE = "ps_5_0";

#ifdef SLEAK_HOT_RELOAD
constexpr std::string_view DX12_STEM_SUFFIX = "_dx12";

/// Returns the file name up to its first '.', minus a trailing _dx12.
std::string StemFromPath(const std::string& path) {
    std::string stem = std::filesystem::path(path).filename().string();
    stem = stem.substr(0, stem.find('.'));
    if (stem.ends_with(DX12_STEM_SUFFIX)) {
        stem.resize(stem.size() - DX12_STEM_SUFFIX.size());
    }
    return stem;
}

/// Returns the live shader registry, leaked to survive static teardown.
std::unordered_set<DirectX12Shader*>& LiveShaders() {
    static auto* shaders = new std::unordered_set<DirectX12Shader*>();
    return *shaders;
}
#endif

}  // namespace

DirectX12Shader::DirectX12Shader(ID3D12Device* device) : m_device(device) {
    assert(m_device != nullptr);
#ifdef SLEAK_HOT_RELOAD
    LiveShaders().insert(this);
#endif
}

DirectX12Shader::~DirectX12Shader() {
#ifdef SLEAK_HOT_RELOAD
    LiveShaders().erase(this);
#endif
}

#ifdef SLEAK_HOT_RELOAD
const std::unordered_set<DirectX12Shader*>& DirectX12Shader::GetLiveShaders() {
    return LiveShaders();
}

bool DirectX12Shader::RecompileStages(
    Microsoft::WRL::ComPtr<ID3DBlob>& vs,
    Microsoft::WRL::ComPtr<ID3DBlob>& ps) const {
    if (m_sources.vertPath.empty() || m_sources.fragPath.empty()) return false;
    return CompileSources(m_sources, vs, ps);
}

void DirectX12Shader::ReplaceStages(Microsoft::WRL::ComPtr<ID3DBlob> vs,
                                    Microsoft::WRL::ComPtr<ID3DBlob> ps,
                                    ID3D12PipelineState* pso) {
    m_vertexShaderBlob = std::move(vs);
    m_pixelShaderBlob = std::move(ps);
    m_pipelineState = pso;
}
#endif

bool DirectX12Shader::compile(const std::string& shaderPath) {
    // Strip .hlsl extension if present, then use _dx12.hlsl
    std::string dx12Path = shaderPath;
    auto hlslPos = dx12Path.rfind(".hlsl");
    if (hlslPos != std::string::npos) {
        dx12Path = dx12Path.substr(0, hlslPos);
    }
    dx12Path += "_dx12.hlsl";

    return BuildStages({dx12Path, VERTEX_ENTRY, dx12Path, PIXEL_ENTRY});
}

bool DirectX12Shader::compile(const std::string& vertPath,
                              const std::string& fragPath) {
    return BuildStages(
        {vertPath, SPLIT_FILE_ENTRY, fragPath, SPLIT_FILE_ENTRY});
}

bool DirectX12Shader::BuildStages(StageSources sources) {
    Microsoft::WRL::ComPtr<ID3DBlob> vs;
    Microsoft::WRL::ComPtr<ID3DBlob> ps;
    if (!CompileSources(sources, vs, ps)) return false;

    m_vertexShaderBlob = std::move(vs);
    m_pixelShaderBlob = std::move(ps);
#ifdef SLEAK_HOT_RELOAD
    m_vertStem = StemFromPath(sources.vertPath);
    m_fragStem = StemFromPath(sources.fragPath);
    m_sources = std::move(sources);
#endif
    return true;
}

bool DirectX12Shader::CompileSources(
    const StageSources& sources, Microsoft::WRL::ComPtr<ID3DBlob>& vs,
    Microsoft::WRL::ComPtr<ID3DBlob>& ps) const {
    if (!compileShader(sources.vertPath, sources.vertEntry, VERTEX_PROFILE,
                       vs)) {
        SLEAK_ERROR("Failed to compile vertex shader {}", sources.vertPath);
        return false;
    }
    if (!compileShader(sources.fragPath, sources.fragEntry, PIXEL_PROFILE,
                       ps)) {
        SLEAK_ERROR("Failed to compile pixel shader {}", sources.fragPath);
        return false;
    }
    return true;
}

void DirectX12Shader::bind() {
    if (!m_pipelineState) return;
    if (m_renderer) {
        m_renderer->BindShaderPipeline(m_pipelineState.Get());
    } else if (m_commandList) {
        m_commandList->SetPipelineState(m_pipelineState.Get());
    }
}

ID3DBlob* DirectX12Shader::getVertexShaderBlob() const {
    return m_vertexShaderBlob.Get();
}

ID3DBlob* DirectX12Shader::getPixelShaderBlob() const {
    return m_pixelShaderBlob.Get();
}

void DirectX12Shader::SetPipelineState(Microsoft::WRL::ComPtr<ID3D12PipelineState> pso) {
    m_pipelineState = pso;
}

ID3D12PipelineState* DirectX12Shader::GetPipelineState() const {
    return m_pipelineState.Get();
}

void DirectX12Shader::SetCommandList(ID3D12GraphicsCommandList* cmdList) {
    m_commandList = cmdList;
}

void DirectX12Shader::SetRenderer(DirectX12Renderer* renderer) {
    m_renderer = renderer;
}

bool DirectX12Shader::compileShader(
    const std::string& filePath, const std::string& entryPoint,
    const std::string& profile, Microsoft::WRL::ComPtr<ID3DBlob>& blob) const {
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
            SLEAK_ERROR("Shader error in {} ({}): {}", filePath, entryPoint,
                        (char*)errorBlob->GetBufferPointer());
        } else {
            SLEAK_ERROR("Shader error in {} ({}): 0x{:08X}", filePath,
                        entryPoint, static_cast<unsigned int>(hr));
        }
        return false;
    }
    return true;
}

}  // namespace RenderEngine
}  // namespace Sleak