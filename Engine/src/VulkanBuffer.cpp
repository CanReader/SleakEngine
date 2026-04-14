#include "../../include/private/Graphics/Vulkan/VulkanBuffer.hpp"
#include <Logger.hpp>
#include <cstring>
#include <stdexcept>

namespace Sleak {
namespace RenderEngine {

// Static batch state
bool VulkanBuffer::s_batchingEnabled = false;
bool VulkanBuffer::s_batchActive = false;
VkCommandBuffer VulkanBuffer::s_batchCommandBuffer = VK_NULL_HANDLE;
VkDevice VulkanBuffer::s_batchDevice = VK_NULL_HANDLE;
VkCommandPool VulkanBuffer::s_batchCommandPool = VK_NULL_HANDLE;
VkQueue VulkanBuffer::s_batchQueue = VK_NULL_HANDLE;
std::vector<VulkanBuffer::PendingStagingCleanup> VulkanBuffer::s_pendingCleanup;

// Static deferred deletion state
std::vector<VulkanBuffer::DeferredBufferDelete> VulkanBuffer::s_deferredDeletions;
uint64_t VulkanBuffer::s_frameNumber = 0;

// Buffer recycling pool
std::vector<VulkanBuffer::PooledBuffer> VulkanBuffer::s_bufferPool;
VkDeviceSize VulkanBuffer::s_poolBytes = 0;

// VRAM tracking
VkDeviceSize VulkanBuffer::s_totalAllocatedBytes = 0;
VkPhysicalDevice VulkanBuffer::s_physicalDeviceGlobal = VK_NULL_HANDLE;

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
            VkDeviceSize stagingAllocSize = 0;
            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_stagingBuffer, m_stagingMemory, &stagingAllocSize);
            if (m_stagingBuffer == VK_NULL_HANDLE) return false;

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

            if (m_buffer == VK_NULL_HANDLE) {
                if (m_stagingBuffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
                    m_stagingBuffer = VK_NULL_HANDLE;
                }
                if (m_stagingMemory != VK_NULL_HANDLE) {
                    vkFreeMemory(m_device, m_stagingMemory, nullptr);
                    m_stagingMemory = VK_NULL_HANDLE;
                    s_totalAllocatedBytes -= stagingAllocSize;
                }
                return false;
            }

            if (data) {
                CopyBuffer(m_stagingBuffer, m_buffer, Size);
            }

            if (s_batchActive) {
                s_pendingCleanup.push_back({m_stagingBuffer, m_stagingMemory, stagingAllocSize});
            } else {
                vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
                vkFreeMemory(m_device, m_stagingMemory, nullptr);
                s_totalAllocatedBytes -= stagingAllocSize;
            }
            m_stagingBuffer = VK_NULL_HANDLE;
            m_stagingMemory = VK_NULL_HANDLE;
            break;
        }
        case BufferType::Index: {
            VkDeviceSize stagingAllocSize = 0;
            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_stagingBuffer, m_stagingMemory, &stagingAllocSize);
            if (m_stagingBuffer == VK_NULL_HANDLE) return false;

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

            if (m_buffer == VK_NULL_HANDLE) {
                if (m_stagingBuffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
                    m_stagingBuffer = VK_NULL_HANDLE;
                }
                if (m_stagingMemory != VK_NULL_HANDLE) {
                    vkFreeMemory(m_device, m_stagingMemory, nullptr);
                    m_stagingMemory = VK_NULL_HANDLE;
                    s_totalAllocatedBytes -= stagingAllocSize;
                }
                return false;
            }

            if (data) {
                CopyBuffer(m_stagingBuffer, m_buffer, Size);
            }

            if (s_batchActive) {
                s_pendingCleanup.push_back({m_stagingBuffer, m_stagingMemory, stagingAllocSize});
            } else {
                vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
                vkFreeMemory(m_device, m_stagingMemory, nullptr);
                s_totalAllocatedBytes -= stagingAllocSize;
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

            if (m_buffer == VK_NULL_HANDLE) return false;

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
        VkBuffer staging;
        VkDeviceMemory stagingMem;
        VkDeviceSize stagingAllocSize = 0;
        CreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem, &stagingAllocSize);

        void* mapped;
        vkMapMemory(m_device, stagingMem, 0, size, 0, &mapped);
        memcpy(mapped, data, size);
        vkUnmapMemory(m_device, stagingMem);

        CopyBuffer(staging, m_buffer, size);

        if (s_batchActive) {
            s_pendingCleanup.push_back({staging, stagingMem, stagingAllocSize});
        } else {
            vkDestroyBuffer(m_device, staging, nullptr);
            vkFreeMemory(m_device, stagingMem, nullptr);
            s_totalAllocatedBytes -= stagingAllocSize;
        }
    }
}

void VulkanBuffer::Cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    // Only flush if THIS buffer has a pending copy in the active batch.
    // Normally old buffers are destroyed before the batch starts (two-pass
    // column rebuild), so this only triggers during scene transitions.
    if (m_pendingInBatch && s_batchActive) {
        FlushPendingCopies();
        m_pendingInBatch = false;
    }

    if (Type == BufferType::Constant && m_mappedData) {
        vkUnmapMemory(m_device, m_memory);
        m_mappedData = nullptr;
    }

    // Staging buffers can be destroyed immediately (not GPU-visible after copy)
    if (m_stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingMemory = VK_NULL_HANDLE;
    }
    // Defer GPU buffer destruction — may still be referenced by in-flight
    // command buffers. Will be recycled or cleaned up after fence wait.
    if (m_buffer != VK_NULL_HANDLE) {
        s_deferredDeletions.push_back({m_buffer, m_memory, m_device,
                                       m_allocSize, m_usage, m_memoryTypeIndex,
                                       s_frameNumber});
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
        m_allocSize = 0;
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
                                VkDeviceMemory& memory,
                                VkDeviceSize* outAllocSize) {
    if (TryRecycleBuffer(size, usage, properties, buffer, memory, outAllocSize)) {
        return;
    }

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

    uint32_t memTypeIdx = FindMemoryType(memRequirements.memoryTypeBits, properties);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memTypeIdx;

    bool allocated = (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) == VK_SUCCESS);

    if (!allocated && !s_bufferPool.empty()) {
        // OOM Step 1: evict the recycling pool (safe — these are past fence).
        SLEAK_WARN("Vulkan alloc failed, evicting {} pooled buffers ({:.1f} MB)",
                   s_bufferPool.size(),
                   static_cast<double>(s_poolBytes) / (1024.0 * 1024.0));
        for (auto& e : s_bufferPool) {
            vkDestroyBuffer(e.device, e.buffer, nullptr);
            if (e.memory != VK_NULL_HANDLE)
                vkFreeMemory(e.device, e.memory, nullptr);
            s_totalAllocatedBytes -= e.allocSize;
        }
        s_bufferPool.clear();
        s_poolBytes = 0;
        allocated = (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) == VK_SUCCESS);
    }

    if (!allocated) {
        // OOM Step 2: flush ALL deferred deletions (may include in-flight buffers).
        SLEAK_WARN("Vulkan alloc still failing, flushing all deferred deletions");
        FlushAllDeferredDeletions();
        allocated = (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) == VK_SUCCESS);
    }

    if (!allocated) {
        SLEAK_ERROR("Failed to allocate Vulkan buffer memory!");
        vkDestroyBuffer(m_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return;
    }

    vkBindBufferMemory(m_device, buffer, memory, 0);

    s_totalAllocatedBytes += memRequirements.size;

    if (outAllocSize) *outAllocSize = memRequirements.size;

    if (&buffer == &m_buffer) {
        m_allocSize = memRequirements.size;
        m_usage = usage;
        m_memoryTypeIndex = memTypeIdx;
    }
}

void VulkanBuffer::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer,
                               VkDeviceSize size) {
    // If a batch is active, record into the shared command buffer
    EnsureBatchStarted(m_device, m_commandPool, m_graphicsQueue);
    if (s_batchActive) {
        VkBufferCopy copyRegion{};
        copyRegion.size = size;
        vkCmdCopyBuffer(s_batchCommandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
        m_pendingInBatch = true;
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
    if (s_batchActive || !s_batchingEnabled) return;

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

    for (auto& pending : s_pendingCleanup) {
        vkDestroyBuffer(s_batchDevice, pending.buffer, nullptr);
        vkFreeMemory(s_batchDevice, pending.memory, nullptr);
        s_totalAllocatedBytes -= pending.allocSize;
    }
    s_pendingCleanup.clear();

    s_batchCommandBuffer = VK_NULL_HANDLE;
    s_batchActive = false;
}

VulkanBuffer::AsyncFlushResult
VulkanBuffer::FlushPendingCopiesAsync(VkSemaphore signalSemaphore) {
    AsyncFlushResult result;

    if (!s_batchActive) return result;

    vkEndCommandBuffer(s_batchCommandBuffer);

    // Submit with semaphore signal — NO fence wait (zero CPU blocking)
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &s_batchCommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSemaphore;

    vkQueueSubmit(s_batchQueue, 1, &submitInfo, VK_NULL_HANDLE);

    // Return everything the caller needs for per-frame deferred cleanup
    result.submitted = true;
    result.stagingBuffers = std::move(s_pendingCleanup);
    result.commandBuffer = s_batchCommandBuffer;
    result.commandPool = s_batchCommandPool;
    result.device = s_batchDevice;

    s_pendingCleanup.clear();
    s_batchCommandBuffer = VK_NULL_HANDLE;
    s_batchActive = false;

    return result;
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

void VulkanBuffer::ProcessDeferredDeletions(uint32_t maxFramesInFlight) {
    // Process ALL eligible buffers each frame to prevent VRAM exhaustion.
    // Old buffers must be freed at least as fast as new ones are created,
    // otherwise the deferred queue grows unbounded and OOMs the GPU.
    // Use swap-and-pop instead of erase() to avoid O(n²) cost
    for (size_t i = 0; i < s_deferredDeletions.size(); ) {
        auto& entry = s_deferredDeletions[i];
        if (s_frameNumber - entry.frameNumber >= maxFramesInFlight) {
            // Recycle into pool only if both count and byte budget allow
            if (s_bufferPool.size() < MAX_POOL_SIZE &&
                entry.allocSize > 0 &&
                s_poolBytes + entry.allocSize <= MAX_POOL_BYTES) {
                s_bufferPool.push_back({entry.buffer, entry.memory, entry.device,
                                        entry.allocSize, entry.usage, entry.memoryTypeIndex});
                s_poolBytes += entry.allocSize;
            } else {
                vkDestroyBuffer(entry.device, entry.buffer, nullptr);
                if (entry.memory != VK_NULL_HANDLE)
                    vkFreeMemory(entry.device, entry.memory, nullptr);
                s_totalAllocatedBytes -= entry.allocSize;
            }
            entry = std::move(s_deferredDeletions.back());
            s_deferredDeletions.pop_back();
        } else {
            ++i;
        }
    }
}

bool VulkanBuffer::TryRecycleBuffer(VkDeviceSize size,
                                     VkBufferUsageFlags usage,
                                     VkMemoryPropertyFlags properties,
                                     VkBuffer& buffer,
                                     VkDeviceMemory& memory,
                                     VkDeviceSize* outAllocSize) {
    // Find a pooled buffer with matching usage and sufficient size.
    // Prefer smallest fitting buffer to minimize waste.
    int bestIdx = -1;
    VkDeviceSize bestSize = UINT64_MAX;
    for (size_t i = 0; i < s_bufferPool.size(); ++i) {
        auto& p = s_bufferPool[i];
        if (p.device == m_device && p.usage == usage &&
            p.allocSize >= size && p.allocSize < bestSize) {
            // Don't recycle if way too large (>4x waste)
            if (p.allocSize <= size * 4) {
                bestIdx = static_cast<int>(i);
                bestSize = p.allocSize;
            }
        }
    }
    if (bestIdx >= 0) {
        auto& p = s_bufferPool[bestIdx];
        buffer = p.buffer;
        memory = p.memory;
        if (outAllocSize) *outAllocSize = p.allocSize;
        if (&buffer == &m_buffer) {
            m_allocSize = p.allocSize;
            m_usage = p.usage;
            m_memoryTypeIndex = p.memoryTypeIndex;
        }
        s_poolBytes -= p.allocSize;
        s_bufferPool[bestIdx] = s_bufferPool.back();
        s_bufferPool.pop_back();
        return true;
    }
    return false;
}

void VulkanBuffer::EvictPoolOverBudget() {
    // Evict the largest entries first until we're under budget.
    while (s_poolBytes > MAX_POOL_BYTES && !s_bufferPool.empty()) {
        // Find largest entry
        size_t maxIdx = 0;
        VkDeviceSize maxSize = 0;
        for (size_t i = 0; i < s_bufferPool.size(); ++i) {
            if (s_bufferPool[i].allocSize > maxSize) {
                maxSize = s_bufferPool[i].allocSize;
                maxIdx = i;
            }
        }
        auto& e = s_bufferPool[maxIdx];
        vkDestroyBuffer(e.device, e.buffer, nullptr);
        if (e.memory != VK_NULL_HANDLE)
            vkFreeMemory(e.device, e.memory, nullptr);
        s_totalAllocatedBytes -= e.allocSize;
        s_poolBytes -= e.allocSize;
        s_bufferPool[maxIdx] = s_bufferPool.back();
        s_bufferPool.pop_back();
    }
}

void VulkanBuffer::FlushAllDeferredDeletions() {
    for (auto& entry : s_deferredDeletions) {
        vkDestroyBuffer(entry.device, entry.buffer, nullptr);
        if (entry.memory != VK_NULL_HANDLE)
            vkFreeMemory(entry.device, entry.memory, nullptr);
        s_totalAllocatedBytes -= entry.allocSize;
    }
    s_deferredDeletions.clear();

    // Also drain the recycling pool
    for (auto& entry : s_bufferPool) {
        vkDestroyBuffer(entry.device, entry.buffer, nullptr);
        if (entry.memory != VK_NULL_HANDLE)
            vkFreeMemory(entry.device, entry.memory, nullptr);
        s_totalAllocatedBytes -= entry.allocSize;
    }
    s_bufferPool.clear();
    s_poolBytes = 0;
}

VkDeviceSize VulkanBuffer::GetTotalAllocatedBytes() {
    return s_totalAllocatedBytes;
}

VkDeviceSize VulkanBuffer::GetDeviceLocalHeapSize() {
    if (s_physicalDeviceGlobal == VK_NULL_HANDLE) return 0;

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(s_physicalDeviceGlobal, &memProps);

    VkDeviceSize totalDeviceLocal = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
        if (memProps.memoryHeaps[i].flags &
            VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            totalDeviceLocal += memProps.memoryHeaps[i].size;
        }
    }
    return totalDeviceLocal;
}

void VulkanBuffer::SetPhysicalDevice(VkPhysicalDevice device) {
    s_physicalDeviceGlobal = device;
}

}  // namespace RenderEngine
}  // namespace Sleak