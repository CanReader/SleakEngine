#include "../../include/private/Graphics/Vulkan/VulkanCubemapTexture.hpp"
#include <Logger.hpp>
#include <stb_image.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace Sleak {
namespace RenderEngine {

VulkanCubemapTexture::VulkanCubemapTexture(VkDevice device,
                                           VkPhysicalDevice physicalDevice,
                                           VkCommandPool commandPool,
                                           VkQueue graphicsQueue)
    : m_device(device),
      m_physicalDevice(physicalDevice),
      m_commandPool(commandPool),
      m_graphicsQueue(graphicsQueue) {}

VulkanCubemapTexture::~VulkanCubemapTexture() {
    Cleanup();
}

bool VulkanCubemapTexture::LoadCubemap(
    const std::array<std::string, 6>& facePaths) {
    Cleanup();

    // Load all 6 faces and verify dimensions match
    struct FaceData {
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
    };
    std::array<FaceData, 6> faces;

    stbi_set_flip_vertically_on_load(false);

    for (int i = 0; i < 6; i++) {
        int channels;
        faces[i].pixels =
            stbi_load(facePaths[i].c_str(), &faces[i].w, &faces[i].h,
                       &channels, 4);  // Force RGBA
        if (!faces[i].pixels) {
            SLEAK_ERROR("VulkanCubemapTexture: Failed to load face {}: {}",
                        i, facePaths[i]);
            for (int j = 0; j < i; j++)
                stbi_image_free(faces[j].pixels);
            return false;
        }
    }

    // All faces must be the same size
    m_width = static_cast<uint32_t>(faces[0].w);
    m_height = static_cast<uint32_t>(faces[0].h);
    for (int i = 1; i < 6; i++) {
        if (faces[i].w != faces[0].w || faces[i].h != faces[0].h) {
            SLEAK_ERROR(
                "VulkanCubemapTexture: Face {} size ({}x{}) doesn't match "
                "face 0 ({}x{})",
                i, faces[i].w, faces[i].h, faces[0].w, faces[0].h);
            for (int j = 0; j < 6; j++)
                stbi_image_free(faces[j].pixels);
            return false;
        }
    }

    VkDeviceSize faceSize =
        static_cast<VkDeviceSize>(m_width) * m_height * 4;
    VkDeviceSize totalSize = faceSize * 6;

    // 1. Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = totalSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &stagingBuffer) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to create staging buffer");
        for (int i = 0; i < 6; i++)
            stbi_image_free(faces[i].pixels);
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
        SLEAK_ERROR(
            "VulkanCubemapTexture: Failed to allocate staging memory");
        for (int i = 0; i < 6; i++)
            stbi_image_free(faces[i].pixels);
        return false;
    }

    vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);

    // 2. Copy all 6 faces to staging buffer sequentially
    void* mapped;
    vkMapMemory(m_device, stagingMemory, 0, totalSize, 0, &mapped);
    for (int i = 0; i < 6; i++) {
        memcpy(static_cast<char*>(mapped) + faceSize * i, faces[i].pixels,
               static_cast<size_t>(faceSize));
    }
    vkUnmapMemory(m_device, stagingMemory);

    // Free CPU pixel data
    for (int i = 0; i < 6; i++)
        stbi_image_free(faces[i].pixels);

    // 3. Create VkImage with 6 array layers and CUBE_COMPATIBLE flag
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_width;
    imageInfo.extent.height = m_height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 6;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_image) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to create cubemap image");
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }

    vkGetImageMemoryRequirements(m_device, m_image, &memReqs);

    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_imageMemory) !=
        VK_SUCCESS) {
        SLEAK_ERROR(
            "VulkanCubemapTexture: Failed to allocate image memory");
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }

    vkBindImageMemory(m_device, m_image, m_imageMemory, 0);

    // 4. Transition to transfer dst, copy all 6 faces, transition to
    // shader read
    TransitionImageLayout(m_image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6);

    // Copy staging buffer to image (one region per face)
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

        std::array<VkBufferImageCopy, 6> regions{};
        for (uint32_t i = 0; i < 6; i++) {
            regions[i].bufferOffset = faceSize * i;
            regions[i].bufferRowLength = 0;
            regions[i].bufferImageHeight = 0;
            regions[i].imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            regions[i].imageSubresource.mipLevel = 0;
            regions[i].imageSubresource.baseArrayLayer = i;
            regions[i].imageSubresource.layerCount = 1;
            regions[i].imageOffset = {0, 0, 0};
            regions[i].imageExtent = {m_width, m_height, 1};
        }

        vkCmdCopyBufferToImage(cmdBuffer, stagingBuffer, m_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6,
                               regions.data());

        vkEndCommandBuffer(cmdBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuffer;

        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmdBuffer);
    }

    TransitionImageLayout(m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6);

    // 5. Cleanup staging
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    // 6. Create cube image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageView) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to create image view");
        return false;
    }

    // 7. Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to create sampler");
        return false;
    }

    SLEAK_INFO("VulkanCubemapTexture: Loaded cubemap ({}x{}, 6 faces)",
               m_width, m_height);
    return true;
}

bool VulkanCubemapTexture::LoadEquirectangular(const std::string& path,
                                                uint32_t faceSize) {
    Cleanup();

    stbi_set_flip_vertically_on_load(false);

    int panW, panH, channels;
    unsigned char* panorama = stbi_load(path.c_str(), &panW, &panH, &channels, 4);
    if (!panorama) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to load panorama: {}", path);
        return false;
    }

    m_width = faceSize;
    m_height = faceSize;

    // Generate 6 face pixel arrays on CPU
    VkDeviceSize faceBytes = static_cast<VkDeviceSize>(faceSize) * faceSize * 4;
    VkDeviceSize totalSize = faceBytes * 6;

    std::vector<unsigned char> allFaces(static_cast<size_t>(totalSize));

    for (int face = 0; face < 6; ++face) {
        unsigned char* faceData = allFaces.data() + face * faceBytes;

        for (uint32_t y = 0; y < faceSize; ++y) {
            for (uint32_t x = 0; x < faceSize; ++x) {
                float u = (2.0f * (x + 0.5f) / faceSize) - 1.0f;
                float v = (2.0f * (y + 0.5f) / faceSize) - 1.0f;

                float dx, dy, dz;
                switch (face) {
                    case 0: dx =  1.0f; dy = -v;    dz = -u;    break; // +X
                    case 1: dx = -1.0f; dy = -v;    dz =  u;    break; // -X
                    case 2: dx =  u;    dy =  1.0f; dz =  v;    break; // +Y
                    case 3: dx =  u;    dy = -1.0f; dz = -v;    break; // -Y
                    case 4: dx =  u;    dy = -v;    dz =  1.0f; break; // +Z
                    case 5: dx = -u;    dy = -v;    dz = -1.0f; break; // -Z
                    default: dx = dy = dz = 0.0f; break;
                }

                float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= len; dy /= len; dz /= len;

                float lon = std::atan2(dz, dx);
                float lat = std::asin(std::clamp(dy, -1.0f, 1.0f));

                float panU = 0.5f + lon / (2.0f * 3.14159265f);
                float panV = 0.5f - lat / 3.14159265f;

                float srcX = panU * (panW - 1);
                float srcY = panV * (panH - 1);
                int x0 = static_cast<int>(srcX);
                int y0 = static_cast<int>(srcY);
                int x1 = std::min(x0 + 1, panW - 1);
                int y1 = std::min(y0 + 1, panH - 1);
                x0 = std::clamp(x0, 0, panW - 1);
                y0 = std::clamp(y0, 0, panH - 1);
                float fx = srcX - x0;
                float fy = srcY - y0;

                size_t idx = (y * faceSize + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    float c00 = panorama[(y0 * panW + x0) * 4 + c];
                    float c10 = panorama[(y0 * panW + x1) * 4 + c];
                    float c01 = panorama[(y1 * panW + x0) * 4 + c];
                    float c11 = panorama[(y1 * panW + x1) * 4 + c];
                    float val = c00 * (1 - fx) * (1 - fy) + c10 * fx * (1 - fy)
                              + c01 * (1 - fx) * fy + c11 * fx * fy;
                    faceData[idx + c] = static_cast<unsigned char>(
                        std::clamp(val, 0.0f, 255.0f));
                }
            }
        }
    }

    stbi_image_free(panorama);

    // Upload via staging buffer — same pattern as LoadCubemap
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = totalSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &stagingBuffer) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to create staging buffer");
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
        SLEAK_ERROR("VulkanCubemapTexture: Failed to allocate staging memory");
        return false;
    }

    vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);

    void* mapped;
    vkMapMemory(m_device, stagingMemory, 0, totalSize, 0, &mapped);
    memcpy(mapped, allFaces.data(), static_cast<size_t>(totalSize));
    vkUnmapMemory(m_device, stagingMemory);

    // Create VkImage with 6 array layers and CUBE_COMPATIBLE flag
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = faceSize;
    imageInfo.extent.height = faceSize;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 6;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_image) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to create cubemap image");
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }

    vkGetImageMemoryRequirements(m_device, m_image, &memReqs);

    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_imageMemory) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to allocate image memory");
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }

    vkBindImageMemory(m_device, m_image, m_imageMemory, 0);

    TransitionImageLayout(m_image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6);

    // Copy staging buffer to image (one region per face)
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

        std::array<VkBufferImageCopy, 6> regions{};
        for (uint32_t i = 0; i < 6; i++) {
            regions[i].bufferOffset = faceBytes * i;
            regions[i].bufferRowLength = 0;
            regions[i].bufferImageHeight = 0;
            regions[i].imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            regions[i].imageSubresource.mipLevel = 0;
            regions[i].imageSubresource.baseArrayLayer = i;
            regions[i].imageSubresource.layerCount = 1;
            regions[i].imageOffset = {0, 0, 0};
            regions[i].imageExtent = {faceSize, faceSize, 1};
        }

        vkCmdCopyBufferToImage(cmdBuffer, stagingBuffer, m_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6,
                               regions.data());

        vkEndCommandBuffer(cmdBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuffer;

        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmdBuffer);
    }

    TransitionImageLayout(m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6);

    // Cleanup staging
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    // Create cube image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageView) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to create image view");
        return false;
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanCubemapTexture: Failed to create sampler");
        return false;
    }

    SLEAK_INFO("VulkanCubemapTexture: Loaded equirectangular panorama ({}x{} face)",
               faceSize, faceSize);
    return true;
}

bool VulkanCubemapTexture::LoadFromMemory(const void* data, uint32_t width,
                                          uint32_t height,
                                          TextureFormat format) {
    (void)data;
    (void)width;
    (void)height;
    (void)format;
    return false;
}

bool VulkanCubemapTexture::LoadFromFile(const std::string& filePath) {
    (void)filePath;
    return false;
}

void VulkanCubemapTexture::Bind(uint32_t slot) const {
    // Binding in Vulkan is handled through descriptor sets
    (void)slot;
}

void VulkanCubemapTexture::Unbind() const {}

void VulkanCubemapTexture::SetFilter(TextureFilter filter) {
    (void)filter;
}

void VulkanCubemapTexture::SetWrapMode(TextureWrapMode wrapMode) {
    (void)wrapMode;
}

void VulkanCubemapTexture::Cleanup() {
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

uint32_t VulkanCubemapTexture::FindMemoryType(
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

    SLEAK_ERROR("VulkanCubemapTexture: Failed to find suitable memory type");
    return 0;
}

void VulkanCubemapTexture::TransitionImageLayout(VkImage image,
                                                  VkImageLayout oldLayout,
                                                  VkImageLayout newLayout,
                                                  uint32_t layerCount) {
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = m_commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuffer;
    vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &cmdBuffer);

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
    barrier.subresourceRange.layerCount = layerCount;

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
