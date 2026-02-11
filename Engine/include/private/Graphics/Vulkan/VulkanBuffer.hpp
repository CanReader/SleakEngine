#ifndef VULKANBUFFER_HPP_
#define VULKANBUFFER_HPP_

#include "../BufferBase.hpp"
#include <vulkan/vulkan.h>

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

private:
    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& memory);

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

    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;

    void* m_mappedData = nullptr;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // VULKANBUFFER_HPP_
