#include "../../include/private/Graphics/Vulkan/VulkanTexture.hpp"
#include <Logger.hpp>
#include <stb_image.h>
#include <cstring>

namespace Sleak {
namespace RenderEngine {

VulkanTexture::VulkanTexture(VkDevice device,
                             VkPhysicalDevice physicalDevice,
                             VkCommandPool commandPool,
                             VkQueue graphicsQueue)
    : m_device(device),
      m_physicalDevice(physicalDevice),
      m_commandPool(commandPool),
      m_graphicsQueue(graphicsQueue) {}

VulkanTexture::~VulkanTexture() {
    Cleanup();
}

bool VulkanTexture::LoadFromMemory(const void* data, uint32_t width,
                                    uint32_t height, TextureFormat format) {
    if (!data || width == 0 || height == 0) return false;

    Cleanup();

    m_width = width;
    m_height = height;
    m_format = format;

    VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

    // 1. Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &stagingBuffer) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanTexture: Failed to create staging buffer");
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, stagingBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &stagingMemory) !=
        VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        SLEAK_ERROR("VulkanTexture: Failed to allocate staging memory");
        return false;
    }

    vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);

    // 2. Copy pixel data to staging buffer
    void* mapped;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped);
    memcpy(mapped, data, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingMemory);

    // 3. Create VkImage
    VkFormat vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    if (format == TextureFormat::BGRA8)
        vkFormat = VK_FORMAT_B8G8R8A8_UNORM;

    if (!CreateImage(width, height, vkFormat,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT)) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }

    // 4. Transition, copy, transition
    TransitionImageLayout(m_image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // Copy staging buffer to image
    {
        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.commandPool = m_commandPool;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = 1;

        VkCommandBuffer cmdBuffer;
        vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &cmdBuffer);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmdBuffer, &beginInfo);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};

        vkCmdCopyBufferToImage(cmdBuffer, stagingBuffer, m_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);

        vkEndCommandBuffer(cmdBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuffer;

        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmdBuffer);
    }

    TransitionImageLayout(m_image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 5. Cleanup staging resources
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    // 6. Create image view and sampler
    if (!CreateImageView(vkFormat)) return false;
    if (!CreateSampler()) return false;

    return true;
}

bool VulkanTexture::LoadFromFile(const std::string& filePath) {
    int w, h, channels;
    unsigned char* pixels =
        stbi_load(filePath.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        SLEAK_ERROR("VulkanTexture: Failed to load image: {}", filePath);
        return false;
    }

    bool result = LoadFromMemory(pixels, static_cast<uint32_t>(w),
                                 static_cast<uint32_t>(h),
                                 TextureFormat::RGBA8);
    stbi_image_free(pixels);
    return result;
}

void VulkanTexture::Bind(uint32_t slot) const {
    // Binding in Vulkan is done through descriptor sets,
    // handled by the renderer
}

void VulkanTexture::Unbind() const {
    // No-op in Vulkan
}

void VulkanTexture::SetFilter(TextureFilter filter) {
    m_filter = filter;
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
        CreateSampler();
        UpdateDescriptorSets();
    }
}

void VulkanTexture::SetWrapMode(TextureWrapMode wrapMode) {
    m_wrapMode = wrapMode;
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
        CreateSampler();
        UpdateDescriptorSets();
    }
}

void VulkanTexture::UpdateDescriptorSets() {
    if (m_descriptorSets.empty() || m_imageView == VK_NULL_HANDLE || m_sampler == VK_NULL_HANDLE)
        return;

    for (auto& set : m_descriptorSets) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = m_imageView;
        imageInfo.sampler = m_sampler;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = set;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
    }
}

void VulkanTexture::Cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_image, nullptr);
        m_image = VK_NULL_HANDLE;
    }
    if (m_imageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_imageMemory, nullptr);
        m_imageMemory = VK_NULL_HANDLE;
    }
}

uint32_t VulkanTexture::FindMemoryType(
    uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) ==
                properties) {
            return i;
        }
    }

    SLEAK_ERROR("VulkanTexture: Failed to find suitable memory type");
    return 0;
}

bool VulkanTexture::CreateImage(uint32_t width, uint32_t height,
                                VkFormat format,
                                VkImageUsageFlags usage) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_image) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanTexture: Failed to create image");
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, m_image, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_imageMemory) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanTexture: Failed to allocate image memory");
        return false;
    }

    vkBindImageMemory(m_device, m_image, m_imageMemory, 0);
    return true;
}

bool VulkanTexture::CreateImageView(VkFormat format) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageView) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanTexture: Failed to create image view");
        return false;
    }
    return true;
}

bool VulkanTexture::CreateSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

    // Filter
    VkFilter vkFilter = VK_FILTER_LINEAR;
    if (m_filter == TextureFilter::Nearest)
        vkFilter = VK_FILTER_NEAREST;
    samplerInfo.magFilter = vkFilter;
    samplerInfo.minFilter = vkFilter;

    // Wrap mode
    VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    switch (m_wrapMode) {
        case TextureWrapMode::ClampToEdge:
            addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            break;
        case TextureWrapMode::ClampToBorder:
            addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            break;
        case TextureWrapMode::Mirror:
            addressMode = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case TextureWrapMode::MirrorClampToEdge:
            addressMode = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            break;
        default:
            break;
    }
    samplerInfo.addressModeU = addressMode;
    samplerInfo.addressModeV = addressMode;
    samplerInfo.addressModeW = addressMode;

    samplerInfo.anisotropyEnable =
        (m_filter == TextureFilter::Anisotropic) ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanTexture: Failed to create sampler");
        return false;
    }
    return true;
}

void VulkanTexture::TransitionImageLayout(VkImage image,
                                           VkImageLayout oldLayout,
                                           VkImageLayout newLayout) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuffer;
    vkAllocateCommandBuffers(m_device, &allocInfo, &cmdBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuffer, &beginInfo);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage;
    VkPipelineStageFlags dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;
        srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmdBuffer, srcStage, dstStage, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    vkEndCommandBuffer(cmdBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmdBuffer);
}

}  // namespace RenderEngine
}  // namespace Sleak
