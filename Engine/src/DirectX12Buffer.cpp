#include "../../include/private/Graphics/DirectX/DirectX12Buffer.hpp"
#include <Graphics/Vertex.hpp>
#include <cassert>

namespace Sleak {
namespace RenderEngine {

DirectX12Buffer::DirectX12Buffer(ID3D12Device* device, size_t size, BufferType type)
    : BufferBase()
{
    assert(device != nullptr);
    m_device = device;
    Size = size;
    Type = type;  // Set the buffer type in the base class
    ConfigureFromBufferType(type);
}

DirectX12Buffer::DirectX12Buffer(ID3D12Device* device, size_t size, 
                               D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES resourceState)
    : BufferBase(),
      m_heapType(heapType),
      m_resourceState(resourceState)
{
    assert(device != nullptr);
    m_device = device;
    Size = size;
}

DirectX12Buffer::DirectX12Buffer(DirectX12Buffer&& other) noexcept
    : BufferBase(std::move(other)),
      m_device(std::move(other.m_device)),
      m_buffer(std::move(other.m_buffer)),
      m_uploadBuffer(std::move(other.m_uploadBuffer)),  // Add upload buffer
      m_commandAllocator(std::move(other.m_commandAllocator)),
      m_commandList(std::move(other.m_commandList)),
      m_heapType(other.m_heapType),
      m_resourceState(other.m_resourceState),
      m_mappedData(other.m_mappedData)
{
    other.m_mappedData = nullptr;
}

DirectX12Buffer& DirectX12Buffer::operator=(DirectX12Buffer&& other) noexcept
{
    if (this != &other) {
        Cleanup();
        
        BufferBase::operator=(std::move(other));
        m_device = std::move(other.m_device);
        m_buffer = std::move(other.m_buffer);
        m_uploadBuffer = std::move(other.m_uploadBuffer);  // Add upload buffer
        m_commandAllocator = std::move(other.m_commandAllocator);
        m_commandList = std::move(other.m_commandList);
        m_heapType = other.m_heapType;
        m_resourceState = other.m_resourceState;
        m_mappedData = other.m_mappedData;
        
        other.m_mappedData = nullptr;
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
    return Initialize(data, static_cast<uint16_t>(Size));
}

bool DirectX12Buffer::Initialize(const void* data, uint16_t size)
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
    // barrier in CreateUploadBuffer transitions to m_resourceState.
    D3D12_RESOURCE_STATES initialState =
        (data && m_heapType == D3D12_HEAP_TYPE_DEFAULT)
            ? D3D12_RESOURCE_STATE_COPY_DEST
            : m_resourceState;
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

    if (data) {
        // If we have initial data, update the buffer
        if (m_heapType == D3D12_HEAP_TYPE_DEFAULT) {
            // For GPU-only buffers, we need to create an upload buffer
            CreateUploadBuffer(data, size);
        } else {
            // For CPU-accessible buffers, we can directly map and update
            //Update(data, size);
        }
    }

    bIsInitialized = true;
    return true;
}

bool DirectX12Buffer::CreateUploadBuffer(const void* data, size_t dataSize)
{
    // Create upload heap for transferring data to the default heap
    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeapProps.CreationNodeMask = 1;
    uploadHeapProps.VisibleNodeMask = 1;
    
    D3D12_RESOURCE_DESC uploadDesc = CreateBufferDesc();
    
    HRESULT hr = m_device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_uploadBuffer));
    
    if (FAILED(hr)) {
        // Error handling
        return false;
    }
    
    // Map the upload buffer
    void* mappedData = nullptr;
    hr = m_uploadBuffer->Map(0, nullptr, &mappedData);
    
    if (FAILED(hr)) {
        // Error handling
        return false;
    }
    
    // Copy data to the upload buffer
    memcpy(mappedData, data, dataSize);
    m_uploadBuffer->Unmap(0, nullptr);
    
    // Initialize command objects (if not already done)
    if (!m_commandAllocator) {
        hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, 
                                            IID_PPV_ARGS(&m_commandAllocator));
        if (FAILED(hr))
            return false;
    }
    
    if (!m_commandList) {
        hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, 
                                        m_commandAllocator.Get(), nullptr, 
                                        IID_PPV_ARGS(&m_commandList));
        if (FAILED(hr))
            return false;
    }
    
    // Copy data from upload buffer to default buffer
    m_commandList->CopyBufferRegion(m_buffer.Get(), 0, m_uploadBuffer.Get(), 0, dataSize);
    
    // Transition resource state if needed
    if (m_resourceState != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = m_buffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = m_resourceState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        
        m_commandList->ResourceBarrier(1, &barrier);
    }
    
    hr = m_commandList->Close();
    if (FAILED(hr))
        return false;
    
    // Note: The command list needs to be executed by the renderer
    // This typically happens in the Update() method or when the renderer flushes commands
    
    return true;
}

void DirectX12Buffer::Update() {
    if (!m_commandList) {
        // Constant buffers on UPLOAD heap don't have their own
        // command list. They are bound through the renderer's
        // command list during ExecuteCommands.
        if (Type == BufferType::Constant)
            return;
        SLEAK_ERROR("Command list is null for buffer Update!");
        return;
    }

    switch (Type) {
        case BufferType::Vertex:
            SetAsVertexBuffer(m_commandList.Get(),0, sizeof(Sleak::Vertex));
        break;
        case BufferType::Index:
            SetAsIndexBuffer(m_commandList.Get(),DXGI_FORMAT_R32_UINT);
        break;
        case BufferType::Constant:
            SetAsConstantBuffer(m_commandList.Get(),0);
        break;
        default:
            SLEAK_ERROR("Unknown buffer type passed!");
    }
}

void DirectX12Buffer::Cleanup()
{
    if (bIsMapped) {
        Unmap();
    }
    
    m_uploadBuffer.Reset();  // Reset upload buffer
    m_buffer.Reset();
    m_commandList.Reset();
    m_commandAllocator.Reset();
    
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

    if (m_heapType == D3D12_HEAP_TYPE_UPLOAD) {
        // CPU-accessible buffer (like constant buffers)
        if (!bIsMapped && !Map())
            return;
            
        memcpy(m_mappedData, data, size);
        // Note: For constant buffers, we typically don't unmap until the buffer is destroyed
        // If you need different behavior, you can adjust this
    }
    else {
        // GPU-only buffer (like vertex/index buffers)
        // Use an upload buffer and command list to update the buffer
        CreateUploadBuffer(data, size);
    }
}

void DirectX12Buffer::SetAsVertexBuffer(ID3D12GraphicsCommandList* commandList, UINT slot, UINT stride, UINT offset) {
    D3D12_VERTEX_BUFFER_VIEW vbView;
    vbView.BufferLocation = m_buffer->GetGPUVirtualAddress() + offset;
    vbView.StrideInBytes = stride;
    vbView.SizeInBytes = static_cast<UINT>(Size - offset);

    commandList->IASetVertexBuffers(slot, 1, &vbView);
}

// New method to set index buffer in a command list
void DirectX12Buffer::SetAsIndexBuffer(ID3D12GraphicsCommandList* commandList, DXGI_FORMAT format, UINT offset) {
    D3D12_INDEX_BUFFER_VIEW ibView;
    ibView.BufferLocation = m_buffer->GetGPUVirtualAddress() + offset;
    ibView.Format = format; // Typically DXGI_FORMAT_R16_UINT or DXGI_FORMAT_R32_UINT
    ibView.SizeInBytes = static_cast<UINT>(Size - offset);

    commandList->IASetIndexBuffer(&ibView);
}

// New method to bind constant buffer to root signature slot
void DirectX12Buffer::SetAsConstantBuffer(ID3D12GraphicsCommandList* commandList, UINT rootParameterIndex) {
    commandList->SetGraphicsRootConstantBufferView(
        rootParameterIndex, 
        m_buffer->GetGPUVirtualAddress()
    );
}

void* DirectX12Buffer::GetData() { 
    return nullptr;
}
    

} // namespace RenderEngine
} // namespace Sleak