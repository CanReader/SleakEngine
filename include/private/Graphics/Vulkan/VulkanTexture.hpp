#ifndef VULKANTEXTURE_HPP_
#define VULKANTEXTURE_HPP_

#include <vulkan/vulkan.h>

#include <Core/OSDef.hpp>
#include <Runtime/Texture.hpp>
#include <functional>
#include <vector>

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
    /// Uploads caller levels with one copy each, or level 0 plus blit mips.
    bool LoadMipChain(const TextureDesc& desc, const TextureMipLevel* levels,
                      uint32_t levelCount) override;
    /// Decodes an image file and uploads it as a new Vulkan image.
    bool LoadFromFile(const std::string& filePath) override;

    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;

    void SetFilter(TextureFilter filter) override;
    void SetWrapMode(TextureWrapMode wrapMode) override;
    void SetLodBias(float bias) override;
    float GetLodBias() const override { return m_lodBias; }
    /// Forces nearest magnification; minification and mips keep SetFilter.
    void SetNearestMagnification(bool enabled) override;

    // Cap generated mip levels (0 = full chain). Set before Load*; used to
    // stop atlas tiles bleeding into each other at distant mips.
    void SetMaxMipLevels(uint32_t levels) { m_maxMipLevels = levels; }

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    TextureFormat GetFormat() const override { return m_format; }
    TextureType GetType() const override { return TextureType::Texture2D; }
    /// True when the image view is an sRGB format.
    bool IsSRGB() const override { return m_srgb; }

    uint64_t GetImGuiTextureID() const override;

    VkImageView GetImageView() const { return m_imageView; }
    VkSampler GetSampler() const { return m_sampler; }

    /// Takes ownership of per-swapchain-image descriptor sets allocated from
    /// pool, releasing any sets the texture held before.
    void SetDescriptorSets(std::vector<VkDescriptorSet> sets,
                           VkDescriptorPool pool);
    const std::vector<VkDescriptorSet>& GetDescriptorSets() const { return m_descriptorSets; }
    bool HasDescriptorSets() const { return !m_descriptorSets.empty(); }

    /// Allocates and writes fresh descriptor sets for a texture.
    using DescriptorWriter = std::function<void(VulkanTexture*)>;
    /// Sets how the texture replaces its descriptor sets after a change.
    void SetDescriptorWriter(DescriptorWriter writer);
    /// True once the renderer has registered a descriptor writer.
    bool HasDescriptorWriter() const {
        return static_cast<bool>(m_descriptorWriter);
    }

    /// Stamps textures released from now on with the serial of the next
    /// frame submission, the last one that may still reference them.
    static void SetReleaseSerial(uint64_t serial);
    /// Destroys released textures whose stamped submission has completed.
    static void DestroyReleased(uint64_t completedSerial);
    /// Destroys every released texture; the caller guarantees the GPU is idle.
    static void DestroyAllReleased();

   private:
    /// Hands the descriptor sets and GPU objects to the release queue so they
    /// outlive any frame still in flight.
    void Cleanup();
    /// Swaps in a sampler built from the current state and retires the old
    /// sampler and its descriptor sets once no frame in flight uses them.
    void RecreateSampler();

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
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    DescriptorWriter m_descriptorWriter;
    mutable VkDescriptorSet m_imguiDescriptorSet = VK_NULL_HANDLE;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGBA8;
    bool m_srgb = false;
    TextureFilter m_filter = TextureFilter::Linear;
    TextureWrapMode m_wrapMode = TextureWrapMode::Repeat;
    float m_lodBias = 0.0f;
    bool m_nearestMagnification = false;
    uint32_t m_mipLevels = 1;      // actual generated levels
    uint32_t m_maxMipLevels = 0;   // 0 = full chain; else clamp
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // VULKANTEXTURE_HPP_
