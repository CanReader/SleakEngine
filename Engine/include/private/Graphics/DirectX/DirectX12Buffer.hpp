#ifndef DIRECTX12BUFFER_HPP_
#define DIRECTX12BUFFER_HPP_
#include "../BufferBase.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>

namespace Sleak {
namespace RenderEngine {
class DirectX12Buffer : public BufferBase {
public:
    // Constructor for common buffer types
    DirectX12Buffer(ID3D12Device* device, size_t size, BufferType type);
   
    // Constructor for custom buffer configuration
    DirectX12Buffer(ID3D12Device* device, size_t size, D3D12_HEAP_TYPE heapType,
                   D3D12_RESOURCE_STATES resourceState);
   
    // Prevent copying
    DirectX12Buffer(const DirectX12Buffer&) = delete;
    DirectX12Buffer& operator=(const DirectX12Buffer&) = delete;
   
    // Allow moving
    DirectX12Buffer(DirectX12Buffer&&) noexcept;
    DirectX12Buffer& operator=(DirectX12Buffer&&) noexcept;
   
    ~DirectX12Buffer() override;
    bool Initialize(void* Data) override;
    void Update() override;
    void Cleanup() override;
   
    bool Map() override;
    void Unmap() override;
    void Update(void* data, size_t size) override;
    bool Initialize(const void* data, uint16_t size);
   
    void* GetData() override;

    // Getter for the underlying D3D buffer
    ID3D12Resource* GetD3DBuffer() const { return m_buffer.Get(); }
    
    // Get command list for execution
    ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }
    
    // Check if command list is ready for execution
    bool HasPendingCommands() const { return m_commandList != nullptr; }
   
    // Additional utility methods
    bool IsValid() const { return m_buffer != nullptr; }
    D3D12_HEAP_TYPE GetHeapType() const { return m_heapType; }
    D3D12_RESOURCE_STATES GetResourceState() const { return m_resourceState; }
    
private:
    // Helper method to set up buffer description
    D3D12_RESOURCE_DESC CreateBufferDesc() const;
   
    // Convert BufferType to DirectX configuration
    void ConfigureFromBufferType(BufferType type);
    
    // Create an upload buffer and copy data to the default buffer
    bool CreateUploadBuffer(const void* data, size_t dataSize);
    
    void SetAsVertexBuffer  (ID3D12GraphicsCommandList* commandList, UINT slot, UINT stride, UINT offset = 0);
    void SetAsIndexBuffer   (ID3D12GraphicsCommandList* commandList, DXGI_FORMAT format, UINT offset = 0);
    void SetAsConstantBuffer(ID3D12GraphicsCommandList* commandList, UINT rootParameterIndex);
    
    // Smart pointers for proper resource management
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;  // Add upload buffer
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
   
    D3D12_HEAP_TYPE m_heapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES m_resourceState = D3D12_RESOURCE_STATE_COMMON;
    void* m_mappedData = nullptr;
};
} // namespace RenderEngine
} // namespace Sleak
#endif // DIRECTX12BUFFER_HPP_