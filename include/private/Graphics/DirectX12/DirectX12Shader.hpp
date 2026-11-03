#ifndef _DIRECTX12_SHADER_H_
#define _DIRECTX12_SHADER_H_

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>
#include <vector>
#ifdef SLEAK_HOT_RELOAD
#include <unordered_set>
#endif

#include "../Common/Shader.hpp"

namespace Sleak {
namespace RenderEngine {
class DirectX12Renderer;

/// D3D12 vertex+pixel shader pair; the pipeline state object itself is owned per-shader.
class DirectX12Shader : public Shader {
public:
    DirectX12Shader(ID3D12Device* device);
    ~DirectX12Shader() override;
    DirectX12Shader(const DirectX12Shader&) = delete;
    DirectX12Shader& operator=(const DirectX12Shader&) = delete;

    /// Compiles the combined-path convention (vs_main/ps_main entry points) from a single HLSL file.
    bool compile(const std::string& shaderPath) override;
    /// Compiles separate vertex and pixel shader HLSL files.
    bool compile(const std::string& vertPath,
                const std::string& fragPath) override;
    /// Binds the shader's cached PSO to the current command list.
    void bind() override;

    ID3DBlob* getVertexShaderBlob() const;
    ID3DBlob* getPixelShaderBlob() const;

    // Per-shader PSO
    void SetPipelineState(Microsoft::WRL::ComPtr<ID3D12PipelineState> pso);
    ID3D12PipelineState* GetPipelineState() const;
    void SetCommandList(ID3D12GraphicsCommandList* cmdList);
    /// Routes bind() through the renderer so pass-specific PSOs are respected.
    void SetRenderer(DirectX12Renderer* renderer);

#ifdef SLEAK_HOT_RELOAD
    /// Recompiles both stages from their files without touching the live ones.
    bool RecompileStages(Microsoft::WRL::ComPtr<ID3DBlob>& vs,
                         Microsoft::WRL::ComPtr<ID3DBlob>& ps) const;
    /// Swaps in new bytecode together with the PSO built from it.
    void ReplaceStages(Microsoft::WRL::ComPtr<ID3DBlob> vs,
                       Microsoft::WRL::ComPtr<ID3DBlob> ps,
                       ID3D12PipelineState* pso);
    /// Returns true when both shaders compile the same files and entry points.
    bool HasSameSources(const DirectX12Shader& other) const {
        return m_sources == other.m_sources;
    }
    /// Returns the stem of the vertex source, e.g. skybox for skybox_dx12.hlsl.
    const std::string& GetStem() const { return m_vertStem; }
    /// Returns true when the vertex or pixel source has the given stem.
    bool UsesStem(const std::string& stem) const {
        return stem == m_vertStem || stem == m_fragStem;
    }
    /// Returns every live DirectX12Shader instance.
    static const std::unordered_set<DirectX12Shader*>& GetLiveShaders();
#endif

   private:
    /// Files and entry points the two stages compile from.
    struct StageSources {
        std::string vertPath;
        std::string vertEntry;
        std::string fragPath;
        std::string fragEntry;

        bool operator==(const StageSources&) const = default;
    };

    /// Compiles both stages and swaps them in only when both succeed.
    bool BuildStages(StageSources sources);
    /// Compiles both stages of the given sources into new blobs.
    bool CompileSources(const StageSources& sources,
                        Microsoft::WRL::ComPtr<ID3DBlob>& vs,
                        Microsoft::WRL::ComPtr<ID3DBlob>& ps) const;
    /// Compiles one HLSL entry point/profile via D3DCompileFromFile into bytecode.
    bool compileShader(const std::string& filePath,
                       const std::string& entryPoint,
                       const std::string& profile,
                       Microsoft::WRL::ComPtr<ID3DBlob>& blob) const;

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;

    Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShaderBlob;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    ID3D12GraphicsCommandList* m_commandList = nullptr;
    DirectX12Renderer* m_renderer = nullptr;
#ifdef SLEAK_HOT_RELOAD
    StageSources m_sources;
    std::string m_vertStem;
    std::string m_fragStem;
#endif
};

}  // namespace RenderEngine
}  // namespace Sleak
#endif  // _DIRECTX12_SHADER_H_