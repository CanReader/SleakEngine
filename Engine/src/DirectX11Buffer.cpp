#include "../../include/private/Graphics/DirectX/DirectX11Buffer.hpp"
#include <Graphics/Vertex.hpp>
#include <cassert>
#include "Graphics/ConstantBuffer.hpp"

namespace Sleak {
namespace RenderEngine {

DirectX11Buffer::DirectX11Buffer(ID3D11Device* device, size_t size, BufferType type)
    : BufferBase()
{
    assert(device != nullptr);
    m_device = device;
    device->GetImmediateContext(m_deviceContext.GetAddressOf());
    Size = size;
    ConfigureFromBufferType(type);
    this->Type = type;
}

DirectX11Buffer::DirectX11Buffer(ID3D11Device* device, size_t size, 
                               D3D11_USAGE usage, UINT bindFlags, UINT cpuAccessFlags)
    : BufferBase(),
      m_usage(usage),
      m_bindFlags(bindFlags),
      m_cpuAccessFlags(cpuAccessFlags)
{
    assert(device != nullptr);
    m_device = device;
    device->GetImmediateContext(m_deviceContext.GetAddressOf());
    Size = size;
}

DirectX11Buffer::DirectX11Buffer(DirectX11Buffer&& other) noexcept
    : BufferBase(std::move(other)),
      m_device(std::move(other.m_device)),
      m_deviceContext(std::move(other.m_deviceContext)),
      m_buffer(std::move(other.m_buffer)),
      m_usage(other.m_usage),
      m_bindFlags(other.m_bindFlags),
      m_cpuAccessFlags(other.m_cpuAccessFlags),
      m_mappedResource(other.m_mappedResource)
{
    other.m_mappedResource = {};
}

DirectX11Buffer& DirectX11Buffer::operator=(DirectX11Buffer&& other) noexcept
{
    if (this != &other) {
        Cleanup();
        
        BufferBase::operator=(std::move(other));
        m_device = std::move(other.m_device);
        m_deviceContext = std::move(other.m_deviceContext);
        m_buffer = std::move(other.m_buffer);
        m_usage = other.m_usage;
        m_bindFlags = other.m_bindFlags;
        m_cpuAccessFlags = other.m_cpuAccessFlags;
        m_mappedResource = other.m_mappedResource;
        
        other.m_mappedResource = {};
    }
    return *this;
}

DirectX11Buffer::~DirectX11Buffer()
{
    Cleanup();
}

void DirectX11Buffer::ConfigureFromBufferType(BufferType type)
{
    switch (type) {
        case BufferType::Vertex:
            m_usage = D3D11_USAGE_DEFAULT;
            m_bindFlags = D3D11_BIND_VERTEX_BUFFER;
            m_cpuAccessFlags = 0;
            break;
            
        case BufferType::Index:
            m_usage = D3D11_USAGE_DEFAULT;
            m_bindFlags = D3D11_BIND_INDEX_BUFFER;
            m_cpuAccessFlags = 0;
            break;
            
        case BufferType::Constant:
            m_usage = D3D11_USAGE_DEFAULT;
            m_bindFlags = D3D11_BIND_CONSTANT_BUFFER;
            m_cpuAccessFlags = 0;
            break;
            
        default:
            m_usage = D3D11_USAGE_DYNAMIC;
            m_bindFlags = D3D11_BIND_SHADER_RESOURCE;
            m_cpuAccessFlags = D3D11_CPU_ACCESS_WRITE;
            break;
    }
}

D3D11_BUFFER_DESC DirectX11Buffer::CreateBufferDesc() const
{
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(Size);
    desc.Usage = m_usage;
    desc.BindFlags = m_bindFlags;
    desc.CPUAccessFlags = m_cpuAccessFlags;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    return desc;
}

bool DirectX11Buffer::Initialize(void* Data) {
    return Initialize(Data,Size);
}

bool DirectX11Buffer::Initialize(const void* data, uint16_t size)
{
    if (!m_device || Size == 0)
        return false;

    if (m_buffer)
        Cleanup();

    D3D11_SUBRESOURCE_DATA resource;
    resource.pSysMem = data;

    auto desc = CreateBufferDesc();
    HRESULT hr = m_device->CreateBuffer(&desc, data != nullptr ? &resource : nullptr, m_buffer.GetAddressOf());
    
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to create DirectX11 buffer! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
        return false;
    }

    bIsInitialized = true;
    return true;
}

void DirectX11Buffer::Update() {
    UINT stride = sizeof(Sleak::Vertex);
    UINT offset = 0;
    
    switch (Type) {
        case BufferType::Vertex:
            m_deviceContext->IASetVertexBuffers(Slot, 1, m_buffer.GetAddressOf(), &stride, &offset);
        break;
        case BufferType::Index:
            m_deviceContext->IASetIndexBuffer(m_buffer.Get(),DXGI_FORMAT_R32_UINT,0);
            break;
        case BufferType::Constant:
            m_deviceContext->VSSetConstantBuffers(Slot,1,m_buffer.GetAddressOf());
            m_deviceContext->PSSetConstantBuffers(Slot,1,m_buffer.GetAddressOf());
            break;
        default:
            SLEAK_ERROR("Unknown buffer type passed!");
    }
}

void DirectX11Buffer::Cleanup()
{
    if (bIsMapped) {
        Unmap();
    }
    
    m_buffer.Reset();
    bIsInitialized = false;
    Size = 0;
    Data = nullptr;
    bIsMapped = false;
}

bool DirectX11Buffer::Map()
{
    if (!m_buffer || !m_deviceContext || bIsMapped)
        return false;

    D3D11_MAP mapType = (m_cpuAccessFlags & D3D11_CPU_ACCESS_READ) 
        ? D3D11_MAP_READ 
        : (m_usage == D3D11_USAGE_DYNAMIC ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE);

    HRESULT hr = m_deviceContext->Map(m_buffer.Get(), 0, mapType, 0, &m_mappedResource);
    
    if (FAILED(hr)) {
        SLEAK_ERROR("Failed to map DirectX11 buffer! HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
        return false;
    }

    Data = m_mappedResource.pData;
    bIsMapped = true;
    return true;
}

void DirectX11Buffer::Unmap()
{
    if (m_buffer && m_deviceContext && bIsMapped) {
        m_deviceContext->Unmap(m_buffer.Get(), 0);
        Data = nullptr;
        bIsMapped = false;
    }
}

#include <Math/Matrix.hpp>
void DirectX11Buffer::Update(void* data, size_t size)
{
    if (!m_buffer || !m_deviceContext || size > Size || !data)
        return;

    if (m_cpuAccessFlags != 0) {
        // CPU-accessible buffer
        if (!bIsMapped && !Map())
            return;

        memcpy(Data, data, size);
        Unmap();
    }
    else {
        // GPU-only buffer
        m_deviceContext->UpdateSubresource(m_buffer.Get(), 0, nullptr, data, 0, 0);
    }
}

void* DirectX11Buffer::GetData() {
    if (!m_buffer || !m_deviceContext) {
        SLEAK_ERROR("Buffer or device context is null!");
        return nullptr;
    }

    // If the buffer is CPU-readable, map it and return the data
    if (m_cpuAccessFlags & D3D11_CPU_ACCESS_READ) {
        if (!bIsMapped && !Map()) {
            SLEAK_ERROR("Failed to map buffer for reading!");
            return nullptr;
        }

        // Return the mapped data
        return Data;
    }

    // If the buffer is GPU-only, create a staging buffer to copy the data
    else {
        // Create a staging buffer description
        D3D11_BUFFER_DESC stagingDesc = {};
        stagingDesc.ByteWidth = static_cast<UINT>(Size);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;  // No bind flags for staging buffers
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        stagingDesc.StructureByteStride = 0;

        // Create the staging buffer
        Microsoft::WRL::ComPtr<ID3D11Buffer> stagingBuffer;
        HRESULT hr =
            m_device->CreateBuffer(&stagingDesc, nullptr, &stagingBuffer);
        if (FAILED(hr)) {
            SLEAK_ERROR("Failed to create staging buffer! HRESULT: 0x{:08X}",
                        static_cast<unsigned int>(hr));
            return nullptr;
        }

        // Copy the data from the GPU buffer to the staging buffer
        m_deviceContext->CopyResource(stagingBuffer.Get(), m_buffer.Get());

        // Map the staging buffer to read the data
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        hr = m_deviceContext->Map(stagingBuffer.Get(), 0, D3D11_MAP_READ, 0,
                                  &mappedResource);
        if (FAILED(hr)) {
            SLEAK_ERROR("Failed to map staging buffer! HRESULT: 0x{:08X}",
                        static_cast<unsigned int>(hr));
            return nullptr;
        }

        // Allocate memory to store the data
        void* data = malloc(Size);
        if (!data) {
            SLEAK_ERROR("Failed to allocate memory for buffer data!");
            m_deviceContext->Unmap(stagingBuffer.Get(), 0);
            return nullptr;
        }

        // Copy the data from the staging buffer to the allocated memory
        memcpy(data, mappedResource.pData, Size);

        // Unmap the staging buffer
        m_deviceContext->Unmap(stagingBuffer.Get(), 0);

        // Return the copied data
        return data;
    }
}

} // namespace RenderEngine
} // namespace Sleak