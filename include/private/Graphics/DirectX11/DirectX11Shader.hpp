#ifndef _DIRECTX11_SHADER_H_
#define _DIRECTX11_SHADER_H_

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "../Common/Shader.hpp"

namespace Sleak {
namespace RenderEngine {

/// D3D11 vertex+pixel shader pair, compiled from HLSL and paired with an input layout.
class DirectX11Shader : public Shader {
   public:
    DirectX11Shader(ID3D11Device* device);
    ~DirectX11Shader() override;

    /// Compiles the combined-path convention (vs_main/ps_main entry points) from a single HLSL file.
    bool compile(const std::string& shaderPath) override;
    /// Compiles separate vertex and pixel shader HLSL files.
    bool compile(const std::string& vertPath,
                 const std::string& fragPath) override;
    void bind() override;

    ID3DBlob* getVertexShaderBlob() const;
    /// Builds the input layout matching DefaultLayout or SkinnedLayout from the vertex shader bytecode.
    ID3D11InputLayout* createInputLayout();
    ID3D11InputLayout* getInputLayout() const { return Layout.Get(); }

   private:
    /// Compiles one HLSL entry point/profile via D3DCompile into a shader object and bytecode blob.
    template <typename T>
    bool compileShader(const std::string& filePath,
                       const std::string& entryPoint,
                       const std::string& profile,
                       Microsoft::WRL::ComPtr<T>& shader,
                       Microsoft::WRL::ComPtr<ID3DBlob>& blob);

   private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deviceContext;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> Layout;

    Microsoft::WRL::ComPtr<ID3DBlob>
        m_vertexShaderBlob;  // For input layout creation
    Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShaderBlob;

    D3D11_INPUT_ELEMENT_DESC DefaultLayout[5] = {
    // Semantic        Index  Format                          Slot  Offset  Class                        Step Rate
    {"POSITION",      0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL",        0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TANGENT",       0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR",         0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD",      0, DXGI_FORMAT_R32G32_FLOAT,       0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };

    D3D11_INPUT_ELEMENT_DESC SkinnedLayout[7] = {
    {"POSITION",      0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL",        0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TANGENT",       0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR",         0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD",      0, DXGI_FORMAT_R32G32_FLOAT,       0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"BLENDINDICES",  0, DXGI_FORMAT_R32G32B32A32_SINT,  0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"BLENDWEIGHT",   0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _DIRECTX11_SHADER_H_