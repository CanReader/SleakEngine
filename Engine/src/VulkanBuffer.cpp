#include "../../include/private/Graphics/Vulkan/VulkanBuffer.hpp"
#include <Logger.hpp>
#include <cstring>
#include <stdexcept>

namespace Sleak {
namespace RenderEngine {

// Static batch state
bool VulkanBuffer::s_batchActive = false;
VkCommandBuffer VulkanBuffer::s_batchCommandBuffer = VK_NULL_HANDLE;
VkDevice VulkanBuffer::s_batchDevice = VK_NULL_HANDLE;
VkCommandPool VulkanBuffer::s_batchCommandPool = VK_NULL_HANDLE;
VkQueue VulkanBuffer::s_batchQueue = VK_NULL_HANDLE;
std::vector<VulkanBuffer::PendingStagingCleanup> VulkanBuffer::s_pendingCleanup;

VulkanBuffer::VulkanBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                           uint32_t size, BufferType type,
                           VkCommandPool commandPool, VkQueue graphicsQueue)
    : m_device(device),
      m_physicalDevice(physicalDevice),
      m_commandPool(commandPool),
      m_graphicsQueue(graphicsQueue) {
    Size = size;
    Type = type;
}

VulkanBuffer::~VulkanBuffer() {
    Cleanup();
}

bool VulkanBuffer::Initialize(void* data) {
    if (Size == 0) return false;

    switch (Type) {
        case BufferType::Vertex: {
            // Create staging buffer
            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_stagingBuffer, m_stagingMemory);

            // Copy data to staging buffer
            if (data) {
                void* mapped;
                vkMapMemory(m_device, m_stagingMemory, 0, Size, 0, &mapped);
                memcpy(mapped, data, Size);
                vkUnmapMemory(m_device, m_stagingMemory);
            }

            // Create device-local buffer
            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_buffer, m_memory);

            // Copy from staging to device-local
            if (data) {
                CopyBuffer(m_stagingBuffer, m_buffer, Size);
            }

            // Defer staging cleanup if batch is active
            if (s_batchActive) {
                s_pendingCleanup.push_back({m_stagingBuffer, m_stagingMemory});
            } else {
                vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
                vkFreeMemory(m_device, m_stagingMemory, nullptr);
            }
            m_stagingBuffer = VK_NULL_HANDLE;
            m_stagingMemory = VK_NULL_HANDLE;
            break;
        }
        case BufferType::Index: {
            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_stagingBuffer, m_stagingMemory);

            if (data) {
                void* mapped;
                vkMapMemory(m_device, m_stagingMemory, 0, Size, 0, &mapped);
                memcpy(mapped, data, Size);
                vkUnmapMemory(m_device, m_stagingMemory);
            }

            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_buffer, m_memory);

            if (data) {
                CopyBuffer(m_stagingBuffer, m_buffer, Size);
            }

            // Defer staging cleanup if batch is active
            if (s_batchActive) {
                s_pendingCleanup.push_back({m_stagingBuffer, m_stagingMemory});
            } else {
                vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
                vkFreeMemory(m_device, m_stagingMemory, nullptr);
            }
            m_stagingBuffer = VK_NULL_HANDLE;
            m_stagingMemory = VK_NULL_HANDLE;
            break;
        }
        case BufferType::Constant: {
            // Constant buffers are host-visible for frequent updates
            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_buffer, m_memory);

            // Persistently map
            vkMapMemory(m_device, m_memory, 0, Size, 0, &m_mappedData);

            if (data) {
                memcpy(m_mappedData, data, Size);
            }
            break;
        }
        default: {
            // Generic buffer
            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_buffer, m_memory);

            if (data) {
                void* mapped;
                vkMapMemory(m_device, m_memory, 0, Size, 0, &mapped);
                memcpy(mapped, data, Size);
                vkUnmapMemory(m_device, m_memory);
            }
            break;
        }
    }

    bIsInitialized = true;
    return true;
}

void VulkanBuffer::Update() {
    // For constant buffers with persistent mapping, nothing extra needed
    // Data is already written to mapped memory
}

void VulkanBuffer::Update(void* data, size_t size) {
    if (!data || size == 0) return;

    if (Type == BufferType::Constant && m_mappedData) {
        // Constant buffer is persistently mapped
        memcpy(m_mappedData, data, size);
    } else if (Type == BufferType::Vertex || Type == BufferType::Index) {
        // Need staging buffer for device-local buffers
        VkBuffer staging;
        VkDeviceMemory stagingMem;
        CreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem);

        void* mapped;
        vkMapMemory(m_device, stagingMem, 0, size, 0, &mapped);
        memcpy(mapped, data, size);
        vkUnmapMemory(m_device, stagingMem);

        CopyBuffer(staging, m_buffer, size);

        // Defer staging cleanup if batch is active
        if (s_batchActive) {
            s_pendingCleanup.push_back({staging, stagingMem});
        } else {
            vkDestroyBuffer(m_device, staging, nullptr);
            vkFreeMemory(m_device, stagingMem, nullptr);
        }
    }
}

void VulkanBuffer::Cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    if (Type == BufferType::Constant && m_mappedData) {
        vkUnmapMemory(m_device, m_memory);
        m_mappedData = nullptr;
    }

    if (m_stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingMemory = VK_NULL_HANDLE;
    }
    if (m_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_buffer, nullptr);
        m_buffer = VK_NULL_HANDLE;
    }
    if (m_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_memory, nullptr);
        m_memory = VK_NULL_HANDLE;
    }

    bIsInitialized = false;
}

bool VulkanBuffer::Map() {
    if (bIsMapped) return true;
    if (Type == BufferType::Constant && m_mappedData) {
        Data = m_mappedData;
        bIsMapped = true;
        return true;
    }
    VkResult result = vkMapMemory(m_device, m_memory, 0, Size, 0, &Data);
    if (result != VK_SUCCESS) return false;
    bIsMapped = true;
    return true;
}

void VulkanBuffer::Unmap() {
    if (!bIsMapped) return;
    // Don't unmap persistent constant buffer mappings
    if (Type == BufferType::Constant && m_mappedData) {
        bIsMapped = false;
        return;
    }
    vkUnmapMemory(m_device, m_memory);
    Data = nullptr;
    bIsMapped = false;
}

void* VulkanBuffer::GetData() {
    return m_mappedData ? m_mappedData : Data;
}

void VulkanBuffer::CreateBuffer(VkDeviceSize size,
                                VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags properties,
                                VkBuffer& buffer,
                                VkDeviceMemory& memory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Failed to create Vulkan buffer!");
        return;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        FindMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate Vulkan buffer memory!");
        return;
    }

    vkBindBufferMemory(m_device, buffer, memory, 0);
}

void VulkanBuffer::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer,
                               VkDeviceSize size) {
    // If a batch is active, record into the shared command buffer
    EnsureBatchStarted(m_device, m_commandPool, m_graphicsQueue);
    if (s_batchActive) {
        VkBufferCopy copyRegion{};
        copyRegion.size = size;
        vkCmdCopyBuffer(s_batchCommandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
        return;
    }

    // Fallback: immediate copy (used when no batch is active, e.g. texture uploads)
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence copyFence;
    vkCreateFence(m_device, &fenceInfo, nullptr, &copyFence);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, copyFence);
    vkWaitForFences(m_device, 1, &copyFence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(m_device, copyFence, nullptr);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
}

void VulkanBuffer::EnsureBatchStarted(VkDevice device, VkCommandPool pool,
                                       VkQueue queue) {
    if (s_batchActive) return;

    s_batchDevice = device;
    s_batchCommandPool = pool;
    s_batchQueue = queue;

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = pool;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(device, &allocInfo, &s_batchCommandBuffer) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate batch transfer command buffer!");
        return;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(s_batchCommandBuffer, &beginInfo) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to begin batch transfer command buffer!");
        vkFreeCommandBuffers(device, pool, 1, &s_batchCommandBuffer);
        s_batchCommandBuffer = VK_NULL_HANDLE;
        return;
    }

    s_batchActive = true;
}

void VulkanBuffer::FlushPendingCopies() {
    if (!s_batchActive) return;

    vkEndCommandBuffer(s_batchCommandBuffer);

    // Single submit + single fence for ALL batched copies
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence batchFence;
    vkCreateFence(s_batchDevice, &fenceInfo, nullptr, &batchFence);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &s_batchCommandBuffer;

    vkQueueSubmit(s_batchQueue, 1, &submitInfo, batchFence);
    vkWaitForFences(s_batchDevice, 1, &batchFence, VK_TRUE, UINT64_MAX);

    // Cleanup
    vkDestroyFence(s_batchDevice, batchFence, nullptr);
    vkFreeCommandBuffers(s_batchDevice, s_batchCommandPool, 1, &s_batchCommandBuffer);

    // Free all deferred staging buffers
    for (auto& pending : s_pendingCleanup) {
        vkDestroyBuffer(s_batchDevice, pending.buffer, nullptr);
        vkFreeMemory(s_batchDevice, pending.memory, nullptr);
    }
    s_pendingCleanup.clear();

    s_batchCommandBuffer = VK_NULL_HANDLE;
    s_batchActive = false;
}

uint32_t VulkanBuffer::FindMemoryType(uint32_t typeFilter,
                                       VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) ==
                properties) {
            return i;
        }
    }

    SLEAK_ERROR("Failed to find suitable memory type!");
    return 0;
}

}  // namespace RenderEngine
}  // namespace Sleak
