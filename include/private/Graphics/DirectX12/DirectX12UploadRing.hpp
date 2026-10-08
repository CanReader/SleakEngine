#ifndef _DIRECTX12_UPLOAD_RING_HPP_
#define _DIRECTX12_UPLOAD_RING_HPP_

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace Sleak {
namespace RenderEngine {

/// Per-frame linear allocator over upload heap pages for transient GPU data.
/// Each frame in flight owns its own pages, which are rewound by BeginFrame.
class DirectX12UploadRing {
   public:
    /// CPU write pointer and matching GPU address of one allocation.
    struct Allocation {
        void* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    };

    DirectX12UploadRing() = default;
    ~DirectX12UploadRing() { Release(); }

    DirectX12UploadRing(const DirectX12UploadRing&) = delete;
    DirectX12UploadRing& operator=(const DirectX12UploadRing&) = delete;

    /// Stores the device and sizes the ring. Pages are created on demand.
    void Initialize(ID3D12Device* device, uint32_t frameCount,
                    uint64_t pageSize);

    /// Rewinds the frame's pages. The GPU must be done with that frame.
    void BeginFrame(uint32_t frameIndex);

    /// Returns 256-byte aligned memory valid until the frame is reused.
    bool Allocate(uint64_t size, Allocation& out);

    /// Unmaps and frees every page. The GPU must be idle.
    void Release();

   private:
    struct Page {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint8_t* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        uint64_t size = 0;
    };

    struct Frame {
        std::vector<Page> pages;
        size_t page = 0;
        uint64_t offset = 0;
    };

    bool CreatePage(uint64_t size, Page& out);

    ID3D12Device* m_device = nullptr;
    std::vector<Frame> m_frames;
    uint32_t m_current = 0;
    uint64_t m_pageSize = 0;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _DIRECTX12_UPLOAD_RING_HPP_
