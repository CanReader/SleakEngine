#ifndef DIRECTX12BUFFER_HPP_
#define DIRECTX12BUFFER_HPP_
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "../Common/BufferBase.hpp"

namespace Sleak {
namespace RenderEngine {
class DirectX12UploadContext;

/// D3D12 buffer wrapper: default-heap resource plus an upload heap for CPU-to-GPU copies.
class DirectX12Buffer : public BufferBase {
public:
 /// Uploads go through the shared uploader when given, else they block.
 DirectX12Buffer(ID3D12Device* device, ID3D12CommandQueue* queue, size_t size,
                 BufferType type, DirectX12UploadContext* uploader = nullptr);

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
 /// Creates the default-heap resource and uploads initial data via a staging
 /// buffer, if any.
 bool Initialize(void* Data) override;
 void Update() override;
 void Cleanup() override;

 /// Maps the buffer for CPU writes (upload-heap buffers only).
 bool Map() override;
 void Unmap() override;
 /// Overwrites buffer contents via a fresh upload-heap copy.
 void Update(void* data, size_t size) override;
 /// Creates the buffer sized and pre-populated from a raw payload.
 bool Initialize(const void* data, size_t size);

 void* GetData() override;

 // Getter for the underlying D3D buffer
 ID3D12Resource* GetD3DBuffer() const { return m_buffer.Get(); }

 /// Latest constant buffer contents, copied into the upload ring on bind.
 const void* GetConstantData() const {
     return m_constantData.empty() ? nullptr : m_constantData.data();
 }
 size_t GetConstantDataSize() const { return m_constantData.size(); }

 /// Ring address already holding the current contents for this frame, or 0.
 D3D12_GPU_VIRTUAL_ADDRESS GetFrameAddress(uint64_t frame) const {
     return m_frameSerial == frame ? m_frameAddress : 0;
 }
 /// Remembers where this frame's copy of the contents lives in the ring.
 void SetFrameAddress(uint64_t frame, D3D12_GPU_VIRTUAL_ADDRESS address) {
     m_frameSerial = frame;
     m_frameAddress = address;
 }

    // Additional utility methods
    bool IsValid() const { return m_buffer != nullptr; }
    D3D12_HEAP_TYPE GetHeapType() const { return m_heapType; }
    D3D12_RESOURCE_STATES GetResourceState() const { return m_resourceState; }

private:
    // Helper method to set up buffer description
    D3D12_RESOURCE_DESC CreateBufferDesc() const;

    // Convert BufferType to DirectX configuration
    void ConfigureFromBufferType(BufferType type);

    /// Records a staged copy into the default-heap buffer.
    bool UploadToDefaultHeap(const void* data, size_t dataSize);

   public:
    /// Queues a GPU object for release once the frame that used it is done.
    static void DeferCleanup(Microsoft::WRL::ComPtr<ID3D12Pageable> object);
    /// Stamps everything queued since the last call with its retire value.
    static void TagDeferredCleanup(uint64_t fenceValue);
    /// Releases every queued object whose fence value has completed.
    static void ProcessDeferredCleanup(uint64_t completedValue);

   private:

    // Smart pointers for proper resource management
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    ID3D12CommandQueue* m_commandQueue = nullptr; // non-owning, for upload execution
    DirectX12UploadContext* m_uploader = nullptr;  // non-owning
    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;

    D3D12_HEAP_TYPE m_heapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES m_resourceState = D3D12_RESOURCE_STATE_COMMON;
    // Actual current GPU resource state (tracked for correct barriers on re-upload)
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_COMMON;
    void* m_mappedData = nullptr;

    std::vector<uint8_t> m_constantData;
    uint64_t m_frameSerial = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_frameAddress = 0;
};
} // namespace RenderEngine
} // namespace Sleak
#endif  // DIRECTX12BUFFER_HPP_
