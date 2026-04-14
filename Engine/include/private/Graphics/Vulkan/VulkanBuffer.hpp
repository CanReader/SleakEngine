#ifndef VULKANBUFFER_HPP_
#define VULKANBUFFER_HPP_

#include "../BufferBase.hpp"
#include <vulkan/vulkan.h>
#include <vector>

namespace Sleak {
namespace RenderEngine {

class ENGINE_API VulkanBuffer : public BufferBase {
public:
    VulkanBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                 uint32_t size, BufferType type,
                 VkCommandPool commandPool, VkQueue graphicsQueue);
    ~VulkanBuffer() override;

    bool Initialize(void* data) override;
    void Update() override;
    void Update(void* data, size_t size) override;
    void Cleanup() override;

    bool Map() override;
    void Unmap() override;

    void* GetData() override;

    VkBuffer GetVkBuffer() const { return m_buffer; }

    // Flush all pending buffer copies in a single batched submission (synchronous).
    static void FlushPendingCopies();

    // Async flush: submit pending copies signaling the given semaphore.
    // No CPU wait — the caller must wait on the semaphore before using data.
    struct PendingStagingCleanup {
        VkBuffer buffer;
        VkDeviceMemory memory;
        VkDeviceSize allocSize = 0;
    };
    struct AsyncFlushResult {
        bool submitted = false;
        std::vector<PendingStagingCleanup> stagingBuffers;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
    };
    static AsyncFlushResult FlushPendingCopiesAsync(VkSemaphore signalSemaphore);

    // Enable/disable batching mode. When disabled, CopyBuffer uses
    // immediate per-buffer submissions (safe during init/scene transitions).
    static void SetBatchingEnabled(bool enabled) { s_batchingEnabled = enabled; }

private:
    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& memory,
                      VkDeviceSize* outAllocSize = nullptr);

    void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer,
                    VkDeviceSize size);

    uint32_t FindMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;

    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkDeviceSize m_allocSize = 0;
    VkBufferUsageFlags m_usage = 0;
    uint32_t m_memoryTypeIndex = 0;

    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;

    void* m_mappedData = nullptr;
    bool m_pendingInBatch = false;

    // --- Batched transfer state ---
    static bool s_batchingEnabled;
    static bool s_batchActive;
    static VkCommandBuffer s_batchCommandBuffer;
    static VkDevice s_batchDevice;
    static VkCommandPool s_batchCommandPool;
    static VkQueue s_batchQueue;
    static std::vector<PendingStagingCleanup> s_pendingCleanup;

    static void EnsureBatchStarted(VkDevice device, VkCommandPool pool,
                                   VkQueue queue);

    // --- Deferred buffer deletion ---
    struct DeferredBufferDelete {
        VkBuffer buffer;
        VkDeviceMemory memory;
        VkDevice device;
        VkDeviceSize allocSize;
        VkBufferUsageFlags usage;
        uint32_t memoryTypeIndex;
        uint64_t frameNumber;
    };
    static std::vector<DeferredBufferDelete> s_deferredDeletions;
    static uint64_t s_frameNumber;

    // --- Buffer recycling pool ---
    struct PooledBuffer {
        VkBuffer buffer;
        VkDeviceMemory memory;
        VkDevice device;
        VkDeviceSize allocSize;
        VkBufferUsageFlags usage;
        uint32_t memoryTypeIndex;
    };
    static std::vector<PooledBuffer> s_bufferPool;
    static VkDeviceSize s_poolBytes;  // total bytes currently in pool
    // 64 MB budget — enough to recycle a few dozen column meshes without
    // hoarding hundreds of MB of dead VRAM on a 6 GB card.
    static constexpr VkDeviceSize MAX_POOL_BYTES = 64 * 1024 * 1024;
    static constexpr size_t MAX_POOL_SIZE = 128;  // hard cap on entry count too

    bool TryRecycleBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties,
                          VkBuffer& buffer, VkDeviceMemory& memory,
                          VkDeviceSize* outAllocSize = nullptr);

    // Evict entries from the pool until it fits within the byte budget.
    static void EvictPoolOverBudget();

public:
    // Called by the renderer each frame after fence wait to safely
    // destroy buffers that are no longer referenced by the GPU.
    static void ProcessDeferredDeletions(uint32_t maxFramesInFlight);
    static void FlushAllDeferredDeletions();
    static void AdvanceDeletionFrame() { s_frameNumber++; }

    // VRAM tracking
    static VkDeviceSize GetTotalAllocatedBytes();
    static VkDeviceSize GetDeviceLocalHeapSize();
    static VkDeviceSize GetDeviceLocalAllocatedBytes();
    static void SetPhysicalDevice(VkPhysicalDevice device);
    static void UntrackAllocation(VkDeviceSize size) { s_totalAllocatedBytes -= size; }

private:
    static VkDeviceSize s_totalAllocatedBytes;
    static VkPhysicalDevice s_physicalDeviceGlobal;
    static VkDeviceSize s_perTypeBytes[VK_MAX_MEMORY_TYPES];
    static bool s_memTypeIsDeviceLocal[VK_MAX_MEMORY_TYPES];
    static uint32_t s_memTypeCount;
    static void EvictPoolForMemType(uint32_t memTypeIdx);
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // VULKANBUFFER_HPP_
