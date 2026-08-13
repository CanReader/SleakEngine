#ifndef _DIRECTX12_SHADER_H_
#define _DIRECTX12_SHADER_H_

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "../Common/Shader.hpp"

namespace Sleak {
namespace RenderEngine {

/// D3D12 vertex+pixel shader pair; the pipeline state object itself is owned per-shader.
class DirectX12Shader : public Shader {
public:
    DirectX12Shader(ID3D12Device* device);
    ~DirectX12Shader() override;

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

private:
    /// Compiles one HLSL entry point/profile via D3DCompileFromFile into bytecode.
    bool compileShader(const std::string& filePath,
                      const std::string& entryPoint,
                      const std::string& profile,
                      Microsoft::WRL::ComPtr<ID3DBlob>& blob);

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;

    Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShaderBlob;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    ID3D12GraphicsCommandList* m_commandList = nullptr;
};

}  // namespace RenderEngine
}  // namespace Sleak
#endif  // _DIRECTX12_SHADER_H_