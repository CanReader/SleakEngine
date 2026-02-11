#ifndef DIRECTX11BUFFER_HPP_
#define DIRECTX11BUFFER_HPP_

#include "../BufferBase.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <wrl/client.h>

namespace Sleak {
namespace RenderEngine {

class DirectX11Buffer : public BufferBase {
public:
    // Constructor for common buffer types
    DirectX11Buffer(ID3D11Device* device, size_t size, BufferType type);
    
    // Constructor for custom buffer configuration
    DirectX11Buffer(ID3D11Device* device, size_t size, D3D11_USAGE usage,
                   UINT bindFlags, UINT cpuAccessFlags);
    
    // Prevent copying
    DirectX11Buffer(const DirectX11Buffer&) = delete;
    DirectX11Buffer& operator=(const DirectX11Buffer&) = delete;
    
    // Allow moving
    DirectX11Buffer(DirectX11Buffer&&) noexcept;
    DirectX11Buffer& operator=(DirectX11Buffer&&) noexcept;
    
    ~DirectX11Buffer() override;

    bool Initialize(void* Data) override;
    void Update() override;
    void Cleanup() override;
    
    bool Map() override;
    void Unmap() override;
    void Update(void* data, size_t size) override;

    bool Initialize(const void* data, uint16_t size);

    void* GetData() override;
    
    // Getter for the underlying D3D buffer
    ID3D11Buffer* GetD3DBuffer() const { return m_buffer.Get(); }
    
    // Additional utility methods
    bool IsValid() const { return m_buffer != nullptr; }
    D3D11_USAGE GetUsage() const { return m_usage; }
    UINT GetBindFlags() const { return m_bindFlags; }
    UINT GetCPUAccessFlags() const { return m_cpuAccessFlags; }

private:
    // Helper method to set up buffer description
    D3D11_BUFFER_DESC CreateBufferDesc() const;
    
    // Convert BufferType to DirectX configuration
    void ConfigureFromBufferType(BufferType type);

    // Smart pointers for proper resource management
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deviceContext;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_buffer;
    
    D3D11_USAGE m_usage = D3D11_USAGE_DEFAULT;
    UINT m_bindFlags = 0;
    UINT m_cpuAccessFlags = 0;
    D3D11_MAPPED_SUBRESOURCE m_mappedResource = {};
};

} // namespace RenderEngine
} // namespace Sleak

#endif // DIRECTX11BUFFER_HPP_