#include <Core/Logger.hpp>
#include <Graphics/DirectX12/DirectX12UploadContext.hpp>
#include <utility>

namespace Sleak {
namespace RenderEngine {

bool DirectX12UploadContext::Initialize(ID3D12Device* device,
                                        ID3D12CommandQueue* queue) {
    Release();
    if (!device || !queue) return false;

    HRESULT hr =
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create upload fence! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return false;
    }

    m_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_event) {
        SLEAK_ERROR("Failed to create upload fence event!");
        m_fence.Reset();
        return false;
    }

    m_device = device;
    m_queue = queue;
    return true;
}

ID3D12GraphicsCommandList* DirectX12UploadContext::GetCommandList() {
    if (m_recording) return m_commandList.Get();
    if (!m_device) return nullptr;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT hr = S_OK;
    if (!m_freeAllocators.empty()) {
        allocator = std::move(m_freeAllocators.back());
        m_freeAllocators.pop_back();
        hr = allocator->Reset();
    } else {
        hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator));
    }
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to prepare upload allocator! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return nullptr;
    }

    if (m_commandList) {
        hr = m_commandList->Reset(allocator.Get(), nullptr);
    } else {
        hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&m_commandList));
        if (SUCCEEDED(hr)) m_commandList->SetName(L"UploadCommandList");
    }
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to open upload command list! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return nullptr;
    }

    m_open.allocator = std::move(allocator);
    m_recording = true;
    return m_commandList.Get();
}

void* DirectX12UploadContext::AllocateStaging(uint64_t size,
                                              ID3D12Resource** resource) {
    if (!resource || size == 0 || !GetCommandList()) return nullptr;

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

    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging));
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create staging buffer! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return nullptr;
    }

    D3D12_RANGE noRead = {0, 0};
    void* mapped = nullptr;
    hr = staging->Map(0, &noRead, &mapped);
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to map staging buffer! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        return nullptr;
    }

    *resource = staging.Get();
    m_open.staging.push_back(std::move(staging));
    return mapped;
}

void DirectX12UploadContext::Submit() {
    if (!m_recording) return;
    m_recording = false;

    HRESULT hr = m_commandList->Close();
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to close upload command list! HRESULT: 0x{:08X}",
                    static_cast<unsigned int>(hr));
        m_open = Batch{};
        return;
    }

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    m_open.fenceValue = ++m_fenceValue;
    m_queue->Signal(m_fence.Get(), m_fenceValue);

    m_inFlight.push_back(std::move(m_open));
    m_open = Batch{};
}

void DirectX12UploadContext::Flush() {
    Submit();
    if (!m_fence) return;
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_event);
        WaitForSingleObject(m_event, INFINITE);
    }
    Retire();
}

void DirectX12UploadContext::Retire() {
    if (!m_fence) return;
    const uint64_t completed = m_fence->GetCompletedValue();

    size_t finished = 0;
    while (finished < m_inFlight.size() &&
           m_inFlight[finished].fenceValue <= completed) {
        m_freeAllocators.push_back(std::move(m_inFlight[finished].allocator));
        ++finished;
    }
    m_inFlight.erase(m_inFlight.begin(), m_inFlight.begin() + finished);
}

void DirectX12UploadContext::Release() {
    if (m_queue && m_fence) Flush();

    m_inFlight.clear();
    m_freeAllocators.clear();
    m_open = Batch{};
    m_recording = false;
    m_commandList.Reset();
    m_fence.Reset();
    if (m_event) {
        CloseHandle(m_event);
        m_event = nullptr;
    }
    m_fenceValue = 0;
    m_device = nullptr;
    m_queue = nullptr;
}

}  // namespace RenderEngine
}  // namespace Sleak
