#ifndef VULKANCUBEMAPTEXTURE_HPP_
#define VULKANCUBEMAPTEXTURE_HPP_

#include <Core/OSDef.hpp>
#include <Runtime/Texture.hpp>
#include <vulkan/vulkan.h>
#include <array>
#include <string>

namespace Sleak {
namespace RenderEngine {

/// Vulkan cubemap texture, loadable from six face images or a single equirectangular panorama.
class ENGINE_API VulkanCubemapTexture : public ::Sleak::Texture {
public:
    VulkanCubemapTexture(VkDevice device, VkPhysicalDevice physicalDevice,
                         VkCommandPool commandPool, VkQueue graphicsQueue);
    ~VulkanCubemapTexture() override;

    /// Loads and uploads 6 face images: +X, -X, +Y, -Y, +Z, -Z.
    bool LoadCubemap(const std::array<std::string, 6>& facePaths);

    /// Load from a single equirectangular panorama and convert to cubemap
    bool LoadEquirectangular(const std::string& path, uint32_t faceSize = 512);

    // Texture interface
    /// Unused for cubemaps; load via LoadCubemap/LoadEquirectangular instead.
    bool LoadFromMemory(const void* data, uint32_t width, uint32_t height,
                        TextureFormat format) override;
    /// Unused for cubemaps; load via LoadCubemap/LoadEquirectangular instead.
    bool LoadFromFile(const std::string& filePath) override;

    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;

    void SetFilter(TextureFilter filter) override;
    void SetWrapMode(TextureWrapMode wrapMode) override;

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    TextureFormat GetFormat() const override { return m_format; }
    TextureType GetType() const override { return TextureType::TextureCube; }

    VkImageView GetImageView() const { return m_imageView; }
    VkSampler GetSampler() const { return m_sampler; }

private:
    void Cleanup();
    /// Finds a physical device memory type matching the filter and required properties.
    uint32_t FindMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);
    /// Records a pipeline barrier moving all 6 cube faces between image layouts.
    void TransitionImageLayout(VkImage image, VkImageLayout oldLayout,
                               VkImageLayout newLayout,
                               uint32_t layerCount);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMemory = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGBA8;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // VULKANCUBEMAPTEXTURE_HPP_
