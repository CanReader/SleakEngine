#ifndef _DIRECTX12_SHADER_H_
#define _DIRECTX12_SHADER_H_

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "../Shader.hpp"

namespace Sleak {
namespace RenderEngine {

class DirectX12Shader : public Shader {
public:
    DirectX12Shader(ID3D12Device* device);
    ~DirectX12Shader() override;

    bool compile(const std::string& shaderPath) override;
    bool compile(const std::string& vertPath,
                const std::string& fragPath) override;
    void bind() override;

    ID3DBlob* getVertexShaderBlob() const;
    ID3DBlob* getPixelShaderBlob() const;

    void SetPSO(Microsoft::WRL::ComPtr<ID3D12PipelineState> pso);
    void SetCommandList(ID3D12GraphicsCommandList* cmdList);

private:
    bool compileShader(const std::string& filePath,
                      const std::string& entryPoint,
                      const std::string& profile,
                      Microsoft::WRL::ComPtr<ID3DBlob>& blob);

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;

    Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShaderBlob;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    ID3D12GraphicsCommandList* m_commandList = nullptr;
};

}  // namespace RenderEngine
}  // namespace Sleak
#endif  // _DIRECTX12_SHADER_H_