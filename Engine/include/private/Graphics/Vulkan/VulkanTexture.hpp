#ifndef VULKANTEXTURE_HPP_
#define VULKANTEXTURE_HPP_

#include <Core/OSDef.hpp>
#include <Runtime/Texture.hpp>
#include <vulkan/vulkan.h>

namespace Sleak {
namespace RenderEngine {

class ENGINE_API VulkanTexture : public Texture {
public:
    VulkanTexture(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkCommandPool commandPool, VkQueue graphicsQueue);
    ~VulkanTexture() override;

    bool LoadFromMemory(const void* data, uint32_t width, uint32_t height,
                        TextureFormat format) override;
    bool LoadFromFile(const std::string& filePath) override;

    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;

    void SetFilter(TextureFilter filter) override;
    void SetWrapMode(TextureWrapMode wrapMode) override;

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    TextureFormat GetFormat() const override { return m_format; }
    TextureType GetType() const override { return TextureType::Texture2D; }

    VkImageView GetImageView() const { return m_imageView; }
    VkSampler GetSampler() const { return m_sampler; }

    // Per-texture descriptor sets (one per swapchain image)
    void SetDescriptorSets(std::vector<VkDescriptorSet> sets) { m_descriptorSets = std::move(sets); }
    const std::vector<VkDescriptorSet>& GetDescriptorSets() const { return m_descriptorSets; }
    bool HasDescriptorSets() const { return !m_descriptorSets.empty(); }

private:
    void Cleanup();

    uint32_t FindMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);
    bool CreateImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageUsageFlags usage);
    bool CreateImageView(VkFormat format);
    bool CreateSampler();
    void TransitionImageLayout(VkImage image, VkImageLayout oldLayout,
                               VkImageLayout newLayout);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMemory = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    std::vector<VkDescriptorSet> m_descriptorSets;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGBA8;
    TextureFilter m_filter = TextureFilter::Linear;
    TextureWrapMode m_wrapMode = TextureWrapMode::Repeat;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // VULKANTEXTURE_HPP_
