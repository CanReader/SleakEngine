// VMA backs all buffer allocations (suballocation from large device blocks).
#include <vulkan/vulkan.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

#include "../../include/private/Graphics/Vulkan/VulkanBuffer.hpp"
#include <Core/Logger.hpp>
#include <cstring>
#include <stdexcept>

namespace Sleak {
namespace RenderEngine {

VmaAllocator VulkanBuffer::s_allocator = VK_NULL_HANDLE;

void VulkanBuffer::InitAllocator(VkInstance instance,
                                 VkPhysicalDevice physicalDevice,
                                 VkDevice device) {
    if (s_allocator != VK_NULL_HANDLE) return;
    VmaAllocatorCreateInfo aci{};
    aci.instance = instance;
    aci.physicalDevice = physicalDevice;
    aci.device = device;
    aci.vulkanApiVersion = VK_API_VERSION_1_1;
    if (vmaCreateAllocator(&aci, &s_allocator) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create VMA allocator!");
        s_allocator = VK_NULL_HANDLE;
    }
}

void VulkanBuffer::DestroyAllocator() {
    if (s_allocator == VK_NULL_HANDLE) return;
    vmaDestroyAllocator(s_allocator);
    s_allocator = VK_NULL_HANDLE;
}

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

// OOM fallback state
static uint32_t g_maxFramesInFlight = 3;      // updated each frame by ProcessDeferredDeletions
static uint64_t g_lastPoolWarnFrame = UINT64_MAX;
static uint64_t g_lastReclaimWarnFrame = UINT64_MAX;

// Recycling bucket granularity, per-frame pool-insertion cap, idle-trim rate.
static constexpr VkDeviceSize kPoolBucket = 64 * 1024;
static constexpr size_t kMaxDeletesPerFrame = 64;
static constexpr size_t kIdleTrimPerFrame = 8;
// Only drain the pool after this many consecutive streaming-idle frames, so
// brief gaps while moving don't starve the pool of its reuse benefit.
static constexpr uint32_t kIdleFramesBeforeTrim = 120;
static uint32_t g_idleFrames = 0;

// Recycling helpers
/// Rounds a size up to the nearest pool bucket so similar-sized buffers can share slots.
static VkDeviceSize BucketSize(VkDeviceSize s) {
    return ((s + kPoolBucket - 1) / kPoolBucket) * kPoolBucket;
}
/// True for vertex/index usages, the only kinds worth pooling for reuse.
static bool UsageIsRecyclable(VkBufferUsageFlags usage) {
    return (usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) != 0;
}
/// True when a buffer is both device-local and a recyclable usage, the pool's eligibility gate.
static bool IsRecyclable(VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    return (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
           UsageIsRecyclable(usage);
}

// Buffer recycling pool
std::vector<VulkanBuffer::PooledBuffer> VulkanBuffer::s_bufferPool;
VkDeviceSize VulkanBuffer::s_poolBytes = 0;

// VRAM tracking
VkDeviceSize VulkanBuffer::s_totalAllocatedBytes = 0;
VkPhysicalDevice VulkanBuffer::s_physicalDeviceGlobal = VK_NULL_HANDLE;
VkDeviceSize VulkanBuffer::s_perTypeBytes[VK_MAX_MEMORY_TYPES] = {};
bool VulkanBuffer::s_memTypeIsDeviceLocal[VK_MAX_MEMORY_TYPES] = {};
uint32_t VulkanBuffer::s_memTypeCount = 0;


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
            uint32_t stagingMemTypeIdx = 0;
            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_stagingBuffer, m_stagingMemory, &stagingAllocSize,
                &stagingMemTypeIdx);
            if (m_stagingBuffer == VK_NULL_HANDLE) return false;

            // Copy data to staging buffer
            if (data) {
                void* mapped;
                vmaMapMemory(s_allocator, m_stagingMemory, &mapped);
                memcpy(mapped, data, Size);
                vmaUnmapMemory(s_allocator, m_stagingMemory);
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
                    vmaDestroyBuffer(s_allocator, m_stagingBuffer,
                                     m_stagingMemory);
                    m_stagingBuffer = VK_NULL_HANDLE;
                    m_stagingMemory = VK_NULL_HANDLE;
                    s_totalAllocatedBytes -= stagingAllocSize;
                    if (stagingMemTypeIdx < s_memTypeCount)
                        s_perTypeBytes[stagingMemTypeIdx] -= stagingAllocSize;
                }
                return false;
            }

            if (data) {
                CopyBuffer(m_stagingBuffer, m_buffer, Size);
            }

            if (s_batchActive) {
                s_pendingCleanup.push_back({m_stagingBuffer, m_stagingMemory, stagingAllocSize, stagingMemTypeIdx});
            } else {
                vmaDestroyBuffer(s_allocator, m_stagingBuffer, m_stagingMemory);
                s_totalAllocatedBytes -= stagingAllocSize;
                if (stagingMemTypeIdx < s_memTypeCount)
                    s_perTypeBytes[stagingMemTypeIdx] -= stagingAllocSize;
            }
            m_stagingBuffer = VK_NULL_HANDLE;
            m_stagingMemory = VK_NULL_HANDLE;
            break;
        }
        case BufferType::Index: {
            VkDeviceSize stagingAllocSize = 0;
            uint32_t stagingMemTypeIdx = 0;
            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_stagingBuffer, m_stagingMemory, &stagingAllocSize,
                &stagingMemTypeIdx);
            if (m_stagingBuffer == VK_NULL_HANDLE) return false;

            if (data) {
                void* mapped;
                vmaMapMemory(s_allocator, m_stagingMemory, &mapped);
                memcpy(mapped, data, Size);
                vmaUnmapMemory(s_allocator, m_stagingMemory);
            }

            CreateBuffer(
                Size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_buffer, m_memory);

            if (m_buffer == VK_NULL_HANDLE) {
                if (m_stagingBuffer != VK_NULL_HANDLE) {
                    vmaDestroyBuffer(s_allocator, m_stagingBuffer,
                                     m_stagingMemory);
                    m_stagingBuffer = VK_NULL_HANDLE;
                    m_stagingMemory = VK_NULL_HANDLE;
                    s_totalAllocatedBytes -= stagingAllocSize;
                    if (stagingMemTypeIdx < s_memTypeCount)
                        s_perTypeBytes[stagingMemTypeIdx] -= stagingAllocSize;
                }
                return false;
            }

            if (data) {
                CopyBuffer(m_stagingBuffer, m_buffer, Size);
            }

            if (s_batchActive) {
                s_pendingCleanup.push_back({m_stagingBuffer, m_stagingMemory, stagingAllocSize, stagingMemTypeIdx});
            } else {
                vmaDestroyBuffer(s_allocator, m_stagingBuffer, m_stagingMemory);
                s_totalAllocatedBytes -= stagingAllocSize;
                if (stagingMemTypeIdx < s_memTypeCount)
                    s_perTypeBytes[stagingMemTypeIdx] -= stagingAllocSize;
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
            vmaMapMemory(s_allocator, m_memory, &m_mappedData);

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
                vmaMapMemory(s_allocator, m_memory, &mapped);
                memcpy(mapped, data, Size);
                vmaUnmapMemory(s_allocator, m_memory);
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
        VmaAllocation stagingMem;
        VkDeviceSize stagingAllocSize = 0;
        uint32_t stagingMemTypeIdx = 0;
        CreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem, &stagingAllocSize, &stagingMemTypeIdx);

        void* mapped;
        vmaMapMemory(s_allocator, stagingMem, &mapped);
        memcpy(mapped, data, size);
        vmaUnmapMemory(s_allocator, stagingMem);

        CopyBuffer(staging, m_buffer, size);

        if (s_batchActive) {
            s_pendingCleanup.push_back({staging, stagingMem, stagingAllocSize, stagingMemTypeIdx});
        } else {
            vmaDestroyBuffer(s_allocator, staging, stagingMem);
            s_totalAllocatedBytes -= stagingAllocSize;
            if (stagingMemTypeIdx < s_memTypeCount)
                s_perTypeBytes[stagingMemTypeIdx] -= stagingAllocSize;
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
        vmaUnmapMemory(s_allocator, m_memory);
        m_mappedData = nullptr;
    }

    // Staging buffers can be destroyed immediately (not GPU-visible after copy)
    if (m_stagingBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(s_allocator, m_stagingBuffer, m_stagingMemory);
        m_stagingBuffer = VK_NULL_HANDLE;
        m_stagingMemory = VK_NULL_HANDLE;
    }
    // Defer GPU buffer destruction — may still be referenced by in-flight
    // command buffers. Will be recycled or cleaned up after fence wait.
    if (m_buffer != VK_NULL_HANDLE) {
        s_deferredDeletions.push_back({m_buffer, m_memory, m_device,
                                       m_allocSize, m_usage, m_memoryTypeIndex,
                                       s_frameNumber, m_bufferSize});
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
        m_allocSize = 0;
        m_bufferSize = 0;
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
    VkResult result = vmaMapMemory(s_allocator, m_memory, &Data);
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
    vmaUnmapMemory(s_allocator, m_memory);
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
                                VmaAllocation& memory,
                                VkDeviceSize* outAllocSize,
                                uint32_t* outMemTypeIdx) {
    // Recyclable device-local vertex/index buffers are bucketed so similar
    // meshes can reuse a freed allocation instead of hitting vkAllocateMemory.
    const bool recyclable = IsRecyclable(usage, properties);
    const VkDeviceSize createSize = recyclable ? BucketSize(size) : size;

    if (recyclable &&
        TryRecycleBuffer(createSize, usage, properties, buffer, memory,
                         outAllocSize)) {
        if (outMemTypeIdx && &buffer == &m_buffer) *outMemTypeIdx = m_memoryTypeIndex;
        return;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = createSize;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_AUTO;
    allocCI.requiredFlags = properties;   // preserve exact memory-property semantics
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        allocCI.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    VmaAllocationInfo info{};
    VkResult r = vmaCreateBuffer(s_allocator, &bufferInfo, &allocCI,
                                 &buffer, &memory, &info);

    if (r != VK_SUCCESS && !s_bufferPool.empty()) {
        // OOM step 1: evict whole recycling pool (pool buffers are not GPU-referenced)
        if (g_lastPoolWarnFrame != s_frameNumber) {
            SLEAK_WARN("Vulkan alloc failed, evicting {} pooled buffers ({:.1f} MB)",
                       s_bufferPool.size(),
                       static_cast<double>(s_poolBytes) / (1024.0 * 1024.0));
            g_lastPoolWarnFrame = s_frameNumber;
        }
        for (auto& e : s_bufferPool) {
            vmaDestroyBuffer(s_allocator, e.buffer, e.memory);
            s_totalAllocatedBytes -= e.allocSize;
            if (e.memoryTypeIndex < s_memTypeCount)
                s_perTypeBytes[e.memoryTypeIndex] -= e.allocSize;
        }
        s_bufferPool.clear();
        s_poolBytes = 0;
        r = vmaCreateBuffer(s_allocator, &bufferInfo, &allocCI,
                            &buffer, &memory, &info);
    }

    if (r != VK_SUCCESS) {
        // OOM step 2: reclaim fence-safe deferred deletions (no device-wide idle)
        if (g_lastReclaimWarnFrame != s_frameNumber) {
            SLEAK_WARN("Vulkan alloc still failing, reclaiming fence-safe deferred deletions");
            g_lastReclaimWarnFrame = s_frameNumber;
        }
        ProcessDeferredDeletions(g_maxFramesInFlight);
        r = vmaCreateBuffer(s_allocator, &bufferInfo, &allocCI,
                            &buffer, &memory, &info);
    }

    if (r != VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate Vulkan buffer memory!");
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return;
    }

    s_totalAllocatedBytes += info.size;
    if (info.memoryType < s_memTypeCount)
        s_perTypeBytes[info.memoryType] += info.size;

    if (outAllocSize) *outAllocSize = info.size;
    if (outMemTypeIdx) *outMemTypeIdx = info.memoryType;

    if (&buffer == &m_buffer) {
        m_allocSize = info.size;
        m_usage = usage;
        m_memoryTypeIndex = info.memoryType;
        m_bufferSize = createSize;
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
        vmaDestroyBuffer(s_allocator, pending.buffer, pending.memory);
        s_totalAllocatedBytes -= pending.allocSize;
        if (pending.memoryTypeIndex < s_memTypeCount)
            s_perTypeBytes[pending.memoryTypeIndex] -= pending.allocSize;
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
    g_maxFramesInFlight = maxFramesInFlight;

    // Reclaim EVERY buffer whose fence chain confirms the GPU is done (aged
    // past frames-in-flight). Destroys are uncapped: vmaDestroyBuffer only
    // returns a suballocation to its VMA block, so even large unload bursts
    // are cheap — and the deferred queue never backlogs (a backlog would keep
    // freed buffers allocated and grow VRAM monotonically while moving). Only
    // pool INSERTIONS are throttled per frame; excess eligible buffers are
    // simply destroyed this frame instead of pooled.
    size_t pooled = 0;
    for (size_t i = 0; i < s_deferredDeletions.size(); ) {
        auto& entry = s_deferredDeletions[i];
        if (s_frameNumber - entry.frameNumber >= maxFramesInFlight) {
            const bool deviceLocal =
                entry.memoryTypeIndex < s_memTypeCount &&
                s_memTypeIsDeviceLocal[entry.memoryTypeIndex];
            const bool poolable =
                deviceLocal && UsageIsRecyclable(entry.usage) &&
                entry.bufferSize > 0 &&
                pooled < kMaxDeletesPerFrame &&
                s_bufferPool.size() < MAX_POOL_SIZE &&
                s_poolBytes + entry.allocSize <= MAX_POOL_BYTES;

            if (poolable) {
                // Keep allocated (still counted in s_totalAllocatedBytes).
                s_bufferPool.push_back({entry.buffer, entry.memory, entry.device,
                                        entry.allocSize, entry.usage,
                                        entry.memoryTypeIndex, entry.bufferSize,
                                        s_frameNumber});
                s_poolBytes += entry.allocSize;
                ++pooled;
            } else {
                vmaDestroyBuffer(s_allocator, entry.buffer, entry.memory);
                s_totalAllocatedBytes -= entry.allocSize;
                if (entry.memoryTypeIndex < s_memTypeCount)
                    s_perTypeBytes[entry.memoryTypeIndex] -= entry.allocSize;
            }
            entry = std::move(s_deferredDeletions.back());
            s_deferredDeletions.pop_back();
        } else {
            ++i;
        }
    }

    EvictPoolOverBudget();

    // Idle drain: only after SUSTAINED inactivity (empty queue for many
    // consecutive frames). Brief gaps while streaming keep the pool intact so
    // it provides reuse during movement; once the player truly stops, the pool
    // gently drains and idle VRAM returns toward baseline. Pool entries are
    // already GPU-safe (aged past frames-in-flight before being pooled).
    if (s_deferredDeletions.empty()) {
        if (g_idleFrames < kIdleFramesBeforeTrim) {
            ++g_idleFrames;
        } else {
            for (size_t n = 0; n < kIdleTrimPerFrame && !s_bufferPool.empty(); ++n) {
                auto& e = s_bufferPool.back();
                vmaDestroyBuffer(s_allocator, e.buffer, e.memory);
                s_totalAllocatedBytes -= e.allocSize;
                s_poolBytes -= e.allocSize;
                if (e.memoryTypeIndex < s_memTypeCount)
                    s_perTypeBytes[e.memoryTypeIndex] -= e.allocSize;
                s_bufferPool.pop_back();
            }
        }
    } else {
        g_idleFrames = 0;
    }

#ifdef DEBUG
    // Rate-limited VMA VRAM observability: logical live vs driver block bytes.
    if (s_allocator && (s_frameNumber % 600) == 0) {
        VmaTotalStatistics vs{};
        vmaCalculateStatistics(s_allocator, &vs);
        SLEAK_LOG(
            "VMA: live {} MB / blocks {} MB | pool {} entries {} MB | deferred {}",
            vs.total.statistics.allocationBytes / (1024 * 1024),
            vs.total.statistics.blockBytes / (1024 * 1024),
            s_bufferPool.size(), s_poolBytes / (1024 * 1024),
            s_deferredDeletions.size());
    }
#endif
}

bool VulkanBuffer::TryRecycleBuffer(VkDeviceSize size,
                                     VkBufferUsageFlags usage,
                                     VkMemoryPropertyFlags properties,
                                     VkBuffer& buffer,
                                     VmaAllocation& memory,
                                     VkDeviceSize* outAllocSize) {
    if (!IsRecyclable(usage, properties) || s_bufferPool.empty()) return false;

    // Best-fit: smallest pooled buffer with exact usage that is big enough.
    size_t best = s_bufferPool.size();
    VkDeviceSize bestSize = 0;
    for (size_t i = 0; i < s_bufferPool.size(); ++i) {
        const auto& e = s_bufferPool[i];
        if (e.usage == usage && e.device == m_device && e.bufferSize >= size) {
            if (best == s_bufferPool.size() || e.bufferSize < bestSize) {
                best = i;
                bestSize = e.bufferSize;
            }
        }
    }
    if (best == s_bufferPool.size()) return false;

    PooledBuffer e = s_bufferPool[best];
    s_bufferPool[best] = s_bufferPool.back();
    s_bufferPool.pop_back();
    s_poolBytes -= e.allocSize;

    // Buffer memory binding is permanent — reuse as-is, do NOT rebind.
    buffer = e.buffer;
    memory = e.memory;
    if (outAllocSize) *outAllocSize = e.allocSize;

    if (&buffer == &m_buffer) {
        m_allocSize = e.allocSize;
        m_usage = e.usage;
        m_memoryTypeIndex = e.memoryTypeIndex;
        m_bufferSize = e.bufferSize;
    }
    return true;
}

void VulkanBuffer::EvictPoolOverBudget() {
    // Evict oldest entries first until under both byte and count budget.
    while ((s_poolBytes > MAX_POOL_BYTES || s_bufferPool.size() > MAX_POOL_SIZE) &&
           !s_bufferPool.empty()) {
        size_t oldestIdx = 0;
        uint64_t oldestFrame = s_bufferPool[0].insertFrame;
        for (size_t i = 1; i < s_bufferPool.size(); ++i) {
            if (s_bufferPool[i].insertFrame < oldestFrame) {
                oldestFrame = s_bufferPool[i].insertFrame;
                oldestIdx = i;
            }
        }
        auto& e = s_bufferPool[oldestIdx];
        vmaDestroyBuffer(s_allocator, e.buffer, e.memory);
        s_totalAllocatedBytes -= e.allocSize;
        s_poolBytes -= e.allocSize;
        if (e.memoryTypeIndex < s_memTypeCount)
            s_perTypeBytes[e.memoryTypeIndex] -= e.allocSize;
        s_bufferPool[oldestIdx] = s_bufferPool.back();
        s_bufferPool.pop_back();
    }
}

void VulkanBuffer::FlushAllDeferredDeletions() {
    for (auto& entry : s_deferredDeletions) {
        vmaDestroyBuffer(s_allocator, entry.buffer, entry.memory);
        s_totalAllocatedBytes -= entry.allocSize;
        if (entry.memoryTypeIndex < s_memTypeCount)
            s_perTypeBytes[entry.memoryTypeIndex] -= entry.allocSize;
    }
    s_deferredDeletions.clear();

    // Also drain the recycling pool
    for (auto& entry : s_bufferPool) {
        vmaDestroyBuffer(s_allocator, entry.buffer, entry.memory);
        s_totalAllocatedBytes -= entry.allocSize;
        if (entry.memoryTypeIndex < s_memTypeCount)
            s_perTypeBytes[entry.memoryTypeIndex] -= entry.allocSize;
    }
    s_bufferPool.clear();
    s_poolBytes = 0;
}

VkDeviceSize VulkanBuffer::GetTotalAllocatedBytes() {
    return s_totalAllocatedBytes;
}

void VulkanBuffer::DumpPerFrameAllocStats() {}

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
    if (device == VK_NULL_HANDLE) return;
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device, &memProps);
    s_memTypeCount = memProps.memoryTypeCount;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        uint32_t heapIdx = memProps.memoryTypes[i].heapIndex;
        s_memTypeIsDeviceLocal[i] = (memProps.memoryHeaps[heapIdx].flags &
                                     VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        s_perTypeBytes[i] = 0;
    }
}

VkDeviceSize VulkanBuffer::GetDeviceLocalAllocatedBytes() {
    VkDeviceSize total = 0;
    for (uint32_t i = 0; i < s_memTypeCount; ++i) {
        if (s_memTypeIsDeviceLocal[i]) total += s_perTypeBytes[i];
    }
    return total;
}

void VulkanBuffer::EvictPoolForMemType(uint32_t memTypeIdx) {
    for (size_t i = 0; i < s_bufferPool.size(); ) {
        auto& e = s_bufferPool[i];
        if (e.memoryTypeIndex == memTypeIdx) {
            vmaDestroyBuffer(s_allocator, e.buffer, e.memory);
            s_totalAllocatedBytes -= e.allocSize;
            s_poolBytes -= e.allocSize;
            if (memTypeIdx < s_memTypeCount)
                s_perTypeBytes[memTypeIdx] -= e.allocSize;
            s_bufferPool[i] = s_bufferPool.back();
            s_bufferPool.pop_back();
        } else {
            ++i;
        }
    }
}

}  // namespace RenderEngine
}  // namespace Sleak