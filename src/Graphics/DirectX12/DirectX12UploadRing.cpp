#include <Core/Logger.hpp>
#include <Graphics/DirectX12/DirectX12UploadRing.hpp>
#include <utility>

namespace Sleak {
namespace RenderEngine {

namespace {
constexpr uint64_t kAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

uint64_t AlignUp(uint64_t value) {
    return (value + kAlignment - 1) & ~(kAlignment - 1);
}
}  // namespace

void DirectX12UploadRing::Initialize(ID3D12Device* device, uint32_t frameCount,
                                     uint64_t pageSize) {
    Release();
    m_device = device;
    m_pageSize = AlignUp(pageSize);
    m_frames.resize(frameCount);
    m_current = 0;
}

void DirectX12UploadRing::BeginFrame(uint32_t frameIndex) {
    if (frameIndex >= m_frames.size()) return;
    m_current = frameIndex;
    m_frames[frameIndex].page = 0;
    m_frames[frameIndex].offset = 0;
}

bool DirectX12UploadRing::Allocate(uint64_t size, Allocation& out) {
    if (!m_device || m_current >= m_frames.size() || size == 0) return false;

    const uint64_t aligned = AlignUp(size);
    Frame& frame = m_frames[m_current];

    while (frame.page < frame.pages.size()) {
        Page& page = frame.pages[frame.page];
        if (frame.offset + aligned <= page.size) {
            out.cpu = page.cpu + frame.offset;
            out.gpu = page.gpu + frame.offset;
            frame.offset += aligned;
            return true;
        }
        ++frame.page;
        frame.offset = 0;
    }

    Page page;
    if (!CreatePage(aligned > m_pageSize ? aligned : m_pageSize, page))
        return false;
    frame.pages.push_back(std::move(page));
    frame.page = frame.pages.size() - 1;

    Page& fresh = frame.pages.back();
    out.cpu = fresh.cpu;
    out.gpu = fresh.gpu;
    frame.offset = aligned;
    return true;
}

void DirectX12UploadRing::Release() {
    for (Frame& frame : m_frames) {
        for (Page& page : frame.pages) {
            if (page.resource && page.cpu) page.resource->Unmap(0, nullptr);
        }
        frame.pages.clear();
        frame.page = 0;
        frame.offset = 0;
    }
    m_frames.clear();
    m_device = nullptr;
    m_current = 0;
}

bool DirectX12UploadRing::CreatePage(uint64_t size, Page& out) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&out.resource));
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create upload ring page! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return false;
    }
    out.resource->SetName(L"UploadRingPage");

    D3D12_RANGE noRead = {0, 0};
    void* mapped = nullptr;
    hr = out.resource->Map(0, &noRead, &mapped);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to map upload ring page! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        out.resource.Reset();
        return false;
    }
    out.cpu = static_cast<uint8_t*>(mapped);
    out.gpu = out.resource->GetGPUVirtualAddress();
    out.size = size;
    return true;
}

}  // namespace RenderEngine
}  // namespace Sleak
