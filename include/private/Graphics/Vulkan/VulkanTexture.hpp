#ifndef VULKANTEXTURE_HPP_
#define VULKANTEXTURE_HPP_

#include <Core/OSDef.hpp>
#include <Runtime/Texture.hpp>
#include <vulkan/vulkan.h>

namespace Sleak {
namespace RenderEngine {

/// Vulkan 2D texture: image + view + sampler, with per-swapchain-image descriptor sets.
class ENGINE_API VulkanTexture : public Texture {
public:
    VulkanTexture(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkCommandPool commandPool, VkQueue graphicsQueue);
    ~VulkanTexture() override;

    /// Uploads raw pixel data through a staging buffer and generates mips.
    bool LoadFromMemory(const void* data, uint32_t width, uint32_t height,
                        TextureFormat format) override;
    /// Decodes an image file and uploads it as a new Vulkan image.
    bool LoadFromFile(const std::string& filePath) override;

    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;

    void SetFilter(TextureFilter filter) override;
    void SetWrapMode(TextureWrapMode wrapMode) override;
    void SetLodBias(float bias) override;
    float GetLodBias() const override { return m_lodBias; }

    // Cap generated mip levels (0 = full chain). Set before Load*; used to
    // stop atlas tiles bleeding into each other at distant mips.
    void SetMaxMipLevels(uint32_t levels) { m_maxMipLevels = levels; }

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    TextureFormat GetFormat() const override { return m_format; }
    TextureType GetType() const override { return TextureType::Texture2D; }

    uint64_t GetImGuiTextureID() const override;

    VkImageView GetImageView() const { return m_imageView; }
    VkSampler GetSampler() const { return m_sampler; }

    // Per-texture descriptor sets (one per swapchain image)
    void SetDescriptorSets(std::vector<VkDescriptorSet> sets) { m_descriptorSets = std::move(sets); }
    const std::vector<VkDescriptorSet>& GetDescriptorSets() const { return m_descriptorSets; }
    bool HasDescriptorSets() const { return !m_descriptorSets.empty(); }

private:
    void Cleanup();
    /// Writes this texture's image view/sampler into each of its per-frame descriptor sets.
    void UpdateDescriptorSets();

    /// Finds a physical device memory type matching the filter and required properties.
    uint32_t FindMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);
    /// Allocates the VkImage and its backing device memory.
    bool CreateImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, uint32_t mipLevels);
    bool CreateImageView(VkFormat format);
    bool CreateSampler();
    /// Records a pipeline barrier transitioning the image between layouts.
    void TransitionImageLayout(VkImage image, VkImageLayout oldLayout,
                               VkImageLayout newLayout);
    /// Blits progressively smaller mip levels from the base image.
    void GenerateMipmaps(VkCommandBuffer cmd, int32_t width, int32_t height);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMemory = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    std::vector<VkDescriptorSet> m_descriptorSets;
    mutable VkDescriptorSet m_imguiDescriptorSet = VK_NULL_HANDLE;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGBA8;
    TextureFilter m_filter = TextureFilter::Linear;
    TextureWrapMode m_wrapMode = TextureWrapMode::Repeat;
    float m_lodBias = 0.0f;
    uint32_t m_mipLevels = 1;      // actual generated levels
    uint32_t m_maxMipLevels = 0;   // 0 = full chain; else clamp
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // VULKANTEXTURE_HPP_
