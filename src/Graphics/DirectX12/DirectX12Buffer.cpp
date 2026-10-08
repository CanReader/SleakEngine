#include "../../include/private/Graphics/DirectX12/DirectX12Buffer.hpp"

#include <Runtime/MeshData.hpp>
#include <cassert>
#include <cstring>

#include "../../include/private/Graphics/DirectX12/DirectX12UploadContext.hpp"

namespace Sleak {
namespace RenderEngine {

DirectX12Buffer::DirectX12Buffer(ID3D12Device* device,
                                 ID3D12CommandQueue* queue, size_t size,
                                 BufferType type,
                                 DirectX12UploadContext* uploader)
    : BufferBase() {
    assert(device != nullptr);
    m_device = device;
    m_commandQueue = queue;
    m_uploader = uploader;
    Size = size;
    Type = type;  // Set the buffer type in the base class
    ConfigureFromBufferType(type);
}

DirectX12Buffer::DirectX12Buffer(ID3D12Device* device, size_t size,
                                 D3D12_HEAP_TYPE heapType,
                                 D3D12_RESOURCE_STATES resourceState)
    : BufferBase(), m_heapType(heapType), m_resourceState(resourceState) {
    assert(device != nullptr);
    m_device = device;
    Size = size;
}

DirectX12Buffer::DirectX12Buffer(DirectX12Buffer&& other) noexcept
    : BufferBase(std::move(other)),
      m_device(std::move(other.m_device)),
      m_commandQueue(other.m_commandQueue),
      m_uploader(other.m_uploader),
      m_buffer(std::move(other.m_buffer)),
      m_heapType(other.m_heapType),
      m_resourceState(other.m_resourceState),
      m_currentState(other.m_currentState),
      m_mappedData(other.m_mappedData),
      m_constantData(std::move(other.m_constantData)),
      m_frameSerial(other.m_frameSerial),
      m_frameAddress(other.m_frameAddress) {
    other.m_commandQueue = nullptr;
    other.m_uploader = nullptr;
    other.m_mappedData = nullptr;
    other.m_frameSerial = 0;
    other.m_frameAddress = 0;
}

DirectX12Buffer& DirectX12Buffer::operator=(DirectX12Buffer&& other) noexcept
{
    if (this != &other) {
        Cleanup();

        BufferBase::operator=(std::move(other));
        m_device = std::move(other.m_device);
        m_commandQueue = other.m_commandQueue;
        m_uploader = other.m_uploader;
        m_buffer = std::move(other.m_buffer);
        m_heapType = other.m_heapType;
        m_resourceState = other.m_resourceState;
        m_currentState = other.m_currentState;
        m_mappedData = other.m_mappedData;
        m_constantData = std::move(other.m_constantData);
        m_frameSerial = other.m_frameSerial;
        m_frameAddress = other.m_frameAddress;

        other.m_commandQueue = nullptr;
        other.m_uploader = nullptr;
        other.m_mappedData = nullptr;
        other.m_frameSerial = 0;
        other.m_frameAddress = 0;
    }
    return *this;
}

DirectX12Buffer::~DirectX12Buffer()
{
    Cleanup();
}

void DirectX12Buffer::ConfigureFromBufferType(BufferType type)
{
    switch (type) {
        case BufferType::Vertex:
            m_heapType = D3D12_HEAP_TYPE_DEFAULT;
            m_resourceState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            break;

        case BufferType::Index:
            m_heapType = D3D12_HEAP_TYPE_DEFAULT;
            m_resourceState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
            break;

        case BufferType::Constant:
            m_heapType = D3D12_HEAP_TYPE_UPLOAD;
            m_resourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
            break;

        case BufferType::ShaderResource:
            m_heapType = D3D12_HEAP_TYPE_DEFAULT;
            m_resourceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            break;

        default:
            m_heapType = D3D12_HEAP_TYPE_DEFAULT;
            m_resourceState = D3D12_RESOURCE_STATE_COMMON;
            break;
    }
}

D3D12_RESOURCE_DESC DirectX12Buffer::CreateBufferDesc() const
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = Size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

bool DirectX12Buffer::Initialize(void* data) {
    return Initialize(data, Size);
}

bool DirectX12Buffer::Initialize(const void* data, size_t size)
{
    if (!m_device || Size == 0)
        return false;

    if (m_buffer)
        Cleanup();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = m_heapType;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    auto desc = CreateBufferDesc();
    // When we have initial data for a DEFAULT heap buffer, create in
    // COPY_DEST state so the upload copy can succeed.  The post-copy
    // barrier in UploadToDefaultHeap transitions to m_resourceState.
    D3D12_RESOURCE_STATES initialState =
        (data && m_heapType == D3D12_HEAP_TYPE_DEFAULT)
            ? D3D12_RESOURCE_STATE_COPY_DEST
            : m_resourceState;
    m_currentState = initialState;
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&m_buffer));

    if (FAILED(hr)) {
        // Error handling code here
        return false;
    }

    if (Type == BufferType::Constant) m_constantData.assign(Size, 0);

    if (data) {
        // If we have initial data, update the buffer
        if (m_heapType == D3D12_HEAP_TYPE_DEFAULT) {
            // For GPU-only buffers, we need to stage the data
            UploadToDefaultHeap(data, size);
        } else {
            // For CPU-accessible buffers, we can directly map and update
            Update(const_cast<void*>(data), size);
        }
    }

    bIsInitialized = true;
    return true;
}

bool DirectX12Buffer::UploadToDefaultHeap(const void* data, size_t dataSize) {
    DirectX12UploadContext local;
    DirectX12UploadContext* uploader = m_uploader;
    if (!uploader) {
        if (!local.Initialize(m_device.Get(), m_commandQueue)) return false;
        uploader = &local;
    }

    ID3D12Resource* staging = nullptr;
    void* mapped = uploader->AllocateStaging(dataSize, &staging);
    if (!mapped) return false;
    memcpy(mapped, data, dataSize);

    ID3D12GraphicsCommandList* commandList = uploader->GetCommandList();

    // If the buffer is NOT in COPY_DEST state (i.e. it was already uploaded
    // once and transitioned to its target state), transition it back to
    // COPY_DEST so the copy can succeed.
    if (m_currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = m_buffer.Get();
        barrier.Transition.StateBefore = m_currentState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        commandList->ResourceBarrier(1, &barrier);
    }

    commandList->CopyBufferRegion(m_buffer.Get(), 0, staging, 0, dataSize);

    // Transition resource to its target state
    if (m_resourceState != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = m_buffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = m_resourceState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        commandList->ResourceBarrier(1, &barrier);
    }

    m_currentState = m_resourceState;

    if (uploader == &local) local.Flush();
    return true;
}

void DirectX12Buffer::Update() {
    // Binding happens through the renderer's command list.
}

void DirectX12Buffer::Cleanup()
{
    if (bIsMapped) {
        Unmap();
    }

    // Defer GPU buffer destruction — may still be referenced by in-flight
    // commands.
    if (m_buffer) {
        DeferCleanup(std::move(m_buffer));
        m_buffer = nullptr;
    }

    m_constantData.clear();
    m_frameSerial = 0;
    m_frameAddress = 0;

    bIsInitialized = false;
    Size = 0;
    Data = nullptr;
    bIsMapped = false;
}

bool DirectX12Buffer::Map()
{
    if (!m_buffer || bIsMapped)
        return false;

    // Only upload heaps can be mapped
    if (m_heapType != D3D12_HEAP_TYPE_UPLOAD && m_heapType != D3D12_HEAP_TYPE_READBACK)
        return false;

    HRESULT hr = m_buffer->Map(0, nullptr, &m_mappedData);

    if (FAILED(hr)) {
        // Error handling
        return false;
    }

    Data = m_mappedData;
    bIsMapped = true;
    return true;
}

void DirectX12Buffer::Unmap()
{
    if (m_buffer && bIsMapped) {
        m_buffer->Unmap(0, nullptr);
        Data = nullptr;
        m_mappedData = nullptr;
        bIsMapped = false;
    }
}

void DirectX12Buffer::Update(void* data, size_t size)
{
    if (!m_buffer || size > Size || !data)
        return;

    if (Type == BufferType::Constant) {
        // Store CPU shadow copy for transform CBs (needed by shadow pass)
        if (size <= 128) StoreCPUShadowCopy(data, size);

        if (m_constantData.size() < size) m_constantData.resize(size);
        memcpy(m_constantData.data(), data, size);
        m_frameSerial = 0;
        m_frameAddress = 0;
        return;
    }

    if (m_heapType == D3D12_HEAP_TYPE_UPLOAD) {
        if (!bIsMapped && !Map()) return;
        memcpy(m_mappedData, data, size);
    } else {
        UploadToDefaultHeap(data, size);
    }
}

void* DirectX12Buffer::GetData() {
    return nullptr;
}

namespace {
struct DeferredObject {
    Microsoft::WRL::ComPtr<ID3D12Pageable> object;
    uint64_t fenceValue = 0;
};

std::vector<Microsoft::WRL::ComPtr<ID3D12Pageable>> s_untagged;
std::vector<DeferredObject> s_tagged;
}  // namespace

void DirectX12Buffer::DeferCleanup(
    Microsoft::WRL::ComPtr<ID3D12Pageable> object) {
    if (object) s_untagged.push_back(std::move(object));
}

void DirectX12Buffer::TagDeferredCleanup(uint64_t fenceValue) {
    for (auto& object : s_untagged)
        s_tagged.push_back({std::move(object), fenceValue});
    s_untagged.clear();
}

void DirectX12Buffer::ProcessDeferredCleanup(uint64_t completedValue) {
    std::erase_if(s_tagged, [completedValue](const DeferredObject& d) {
        return d.fenceValue <= completedValue;
    });
}

} // namespace RenderEngine
}  // namespace Sleak
