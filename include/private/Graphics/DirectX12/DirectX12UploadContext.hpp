#ifndef _DIRECTX12_UPLOAD_CONTEXT_HPP_
#define _DIRECTX12_UPLOAD_CONTEXT_HPP_

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace Sleak {
namespace RenderEngine {

/// Batches CPU to GPU copies into one command list on the direct queue.
/// Staging memory is freed once the batch that used it is done on the GPU.
class DirectX12UploadContext {
   public:
    DirectX12UploadContext() = default;
    ~DirectX12UploadContext() { Release(); }

    DirectX12UploadContext(const DirectX12UploadContext&) = delete;
    DirectX12UploadContext& operator=(const DirectX12UploadContext&) = delete;

    /// Creates the fence used to track submitted batches.
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);

    /// Returns the open copy command list, starting a new batch if needed.
    ID3D12GraphicsCommandList* GetCommandList();

    /// Returns a mapped upload buffer that lives until the open batch is done.
    void* AllocateStaging(uint64_t size, ID3D12Resource** resource);

    /// Executes the recorded copies without waiting for them.
    void Submit();

    /// Submits and blocks until every batch has finished on the GPU.
    void Flush();

    /// Frees staging memory of batches the GPU has finished.
    void Retire();

    /// Flushes outstanding work and frees everything.
    void Release();

   private:
    struct Batch {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> staging;
        uint64_t fenceValue = 0;
    };

    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_event = nullptr;
    uint64_t m_fenceValue = 0;

    Batch m_open;
    bool m_recording = false;
    std::vector<Batch> m_inFlight;
    std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>>
        m_freeAllocators;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _DIRECTX12_UPLOAD_CONTEXT_HPP_
