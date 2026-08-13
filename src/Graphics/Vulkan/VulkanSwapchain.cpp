#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"

#include <Core/Window.hpp>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include "Core/Logger.hpp"
#include "SDL3/SDL_video.h"

namespace Sleak {
    namespace RenderEngine {

/// Creates the swapchain from the queried surface capabilities.
bool VulkanRenderer::CreateSwapChain() {
    auto details = QuerySwapchain();
    if (!details.has_value())
        return false;

    if (details->formats.empty() && details->presentModes.empty())
        SLEAK_RETURN_ERR("Swapchain is not supported for this GPU!");

    auto format = ChooseFormat(details->formats);
    auto mode = ChoosePresentMode(details->presentModes);
    auto extent = ChooseExtend(details.value());

    uint32_t imageCount = details->caps.minImageCount + 1;
    if (details->caps.maxImageCount > 0 &&
        imageCount > details->caps.maxImageCount)
        imageCount = details->caps.maxImageCount;

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.minImageCount = imageCount;
    info.imageColorSpace = format.colorSpace;
    info.imageFormat = format.format;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.presentMode = mode;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t indices[] = {QueueIDs.GraphicsIndex, QueueIDs.PresentIndex};

    if (QueueIDs.GraphicsIndex != QueueIDs.PresentIndex) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = indices;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    info.surface = surface;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.preTransform = details->caps.currentTransform;
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    VkResult result =
        vkCreateSwapchainKHR(device, &info, nullptr, &swapChain);
    if (result != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create swap chain for renderer!")

    uint32_t scImageCount = 0;
    vkGetSwapchainImagesKHR(device, swapChain, &scImageCount, nullptr);
    swapChainImages.resize(scImageCount);
    vkGetSwapchainImagesKHR(device, swapChain, &scImageCount,
                             swapChainImages.data());

    scImageFormat = format.format;
    scExtent = extent;

    return true;
}

/// Destroys the swapchain, its image views, and framebuffers.
void VulkanRenderer::CleanupSwapChain() {
    CleanupDepthResources();
    CleanupMSAAColorResources();

    for (auto framebuffer : swapChainFramebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    swapChainFramebuffers.clear();

    for (auto imageView : swapChainImageViews) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    swapChainImageViews.clear();

    if (swapChain) {
        vkDestroySwapchainKHR(device, swapChain, nullptr);
        swapChain = VK_NULL_HANDLE;
    }
}

/// Creates the MSAA color image used as the multisampled render target.
bool VulkanRenderer::CreateMSAAColorResources() {
    if (m_msaaSamples == VK_SAMPLE_COUNT_1_BIT)
        return true; // No MSAA image needed

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = scExtent.width;
    imageInfo.extent.height = scExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = scImageFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.samples = m_msaaSamples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &m_msaaColorImage) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create MSAA color image!");

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, m_msaaColorImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &m_msaaColorImageMemory) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to allocate MSAA color image memory!");

    vkBindImageMemory(device, m_msaaColorImage, m_msaaColorImageMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_msaaColorImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = scImageFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_msaaColorImageView) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create MSAA color image view!");

    return true;
}

/// Destroys the MSAA color image, view, and memory.
void VulkanRenderer::CleanupMSAAColorResources() {
    if (m_msaaColorImageView) {
        vkDestroyImageView(device, m_msaaColorImageView, nullptr);
        m_msaaColorImageView = VK_NULL_HANDLE;
    }
    if (m_msaaColorImage) {
        vkDestroyImage(device, m_msaaColorImage, nullptr);
        m_msaaColorImage = VK_NULL_HANDLE;
    }
    if (m_msaaColorImageMemory) {
        vkFreeMemory(device, m_msaaColorImageMemory, nullptr);
        m_msaaColorImageMemory = VK_NULL_HANDLE;
    }
}

/// Queries the highest MSAA sample count the GPU supports.
VkSampleCountFlagBits VulkanRenderer::GetMaxUsableSampleCount() {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts
                              & props.limits.framebufferDepthSampleCounts;
    if (counts & VK_SAMPLE_COUNT_8_BIT) return VK_SAMPLE_COUNT_8_BIT;
    if (counts & VK_SAMPLE_COUNT_4_BIT) return VK_SAMPLE_COUNT_4_BIT;
    if (counts & VK_SAMPLE_COUNT_2_BIT) return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

/// Creates an image view for each swapchain image.
bool VulkanRenderer::CreateImageViews() {
    swapChainImageViews.resize(swapChainImages.size());

    for (size_t i = 0; i < swapChainImageViews.size(); i++) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = swapChainImages[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = scImageFormat;

        info.components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY};

        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = 0;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = 1;

        auto result = vkCreateImageView(device, &info, nullptr,
                                         &swapChainImageViews[i]);
        if (result != VK_SUCCESS)
            SLEAK_RETURN_ERR(
                "Failed to create image view of swapchain, index: {}", i);
    }

    return true;
}

/// Creates the depth image, memory, and image view.
bool VulkanRenderer::CreateDepthResources() {
    depthFormat = FindDepthFormat();

    // Create depth image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = scExtent.width;
    imageInfo.extent.height = scExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = depthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Always add SAMPLED_BIT so deferred lighting pass can read depth.
    // When deferred is enabled, force 1x samples (GBuffer is non-MSAA).
    VkSampleCountFlagBits depthSamples = m_deferredEnabled
                                        ? VK_SAMPLE_COUNT_1_BIT : m_msaaSamples;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                    | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = depthSamples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &depthImage) !=
        VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create depth image!");

    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, depthImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &depthImageMemory) !=
        VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to allocate depth image memory!");

    vkBindImageMemory(device, depthImage, depthImageMemory, 0);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &depthImageView) !=
        VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create depth image view!");

    return true;
}

/// Destroys the depth image, memory, and image view.
void VulkanRenderer::CleanupDepthResources() {
    if (depthImageView) {
        vkDestroyImageView(device, depthImageView, nullptr);
        depthImageView = VK_NULL_HANDLE;
    }
    if (depthImage) {
        vkDestroyImage(device, depthImage, nullptr);
        depthImage = VK_NULL_HANDLE;
    }
    if (depthImageMemory) {
        vkFreeMemory(device, depthImageMemory, nullptr);
        depthImageMemory = VK_NULL_HANDLE;
    }
}

/// Picks the first supported depth-stencil format from the candidate list.
VkFormat VulkanRenderer::FindDepthFormat() {
    std::vector<VkFormat> candidates = {VK_FORMAT_D32_SFLOAT,
                                         VK_FORMAT_D32_SFLOAT_S8_UINT,
                                         VK_FORMAT_D24_UNORM_S8_UINT};

    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

        if (props.optimalTilingFeatures &
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return format;
        }
    }

    return VK_FORMAT_D32_SFLOAT;  // fallback
}

/// Finds a memory type index matching the filter and property flags.
uint32_t VulkanRenderer::FindMemoryType(
    uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

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

/// Queries surface capabilities, formats, and present modes.
std::optional<SwapchainDetails> VulkanRenderer::QuerySwapchain() {
    SwapchainDetails details;

    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice, surface, &details.caps);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("Failed to retrieve surface information!");
        return {};
    }

    // Fixed: use resize() not reserve()
    uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface,
                                                   &formatCount, nullptr);
    if (result != VK_SUCCESS || formatCount < 1) {
        SLEAK_ERROR("Failed to retrieve supported surface formats");
        return {};
    }

    details.formats.resize(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface,
                                          &formatCount,
                                          details.formats.data());

    // Get surface present modes
    uint32_t modeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physicalDevice, surface, &modeCount, nullptr);
    if (result != VK_SUCCESS || modeCount < 1) {
        SLEAK_ERROR("Failed to retrieve present modes!");
        return {};
    }

    details.presentModes.resize(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                               &modeCount,
                                               details.presentModes.data());

    return details;
}

/// Picks a UNORM surface format to avoid double sRGB encoding.
VkSurfaceFormatKHR VulkanRenderer::ChooseFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) {
    // Prefer UNORM so the GPU does NOT apply automatic sRGB gamma encoding
    // on output. The game renders in sRGB/gamma space already (no linear
    // pipeline), so using _SRGB would gamma-encode everything twice —
    // producing a washed-out, overbright image. _UNORM writes values as-is,
    // matching the behaviour of DX11/DX12 DXGI_FORMAT_*_UNORM swap chains.
    for (auto& format : formats)
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;

    // Second preference: R8G8B8A8_UNORM
    for (auto& format : formats)
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;

    return formats[0];
}

/// Picks FIFO when VSync is on, otherwise MAILBOX or IMMEDIATE.
VkPresentModeKHR VulkanRenderer::ChoosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) {
    if (m_vsync) {
        // VSync ON: FIFO is guaranteed and provides VSync
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // VSync OFF: prefer MAILBOX (no tearing, uncapped), then IMMEDIATE
    for (auto& mode : modes)
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            return mode;
    for (auto& mode : modes)
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            return mode;

    return VK_PRESENT_MODE_FIFO_KHR;
}

/// Clamps the window size to the surface's supported extent.
// Fixed: clamp height using height, not width
VkExtent2D VulkanRenderer::ChooseExtend(SwapchainDetails details) {
    if (details.caps.currentExtent.width !=
        std::numeric_limits<uint32_t>::max())
        return details.caps.currentExtent;

    int width, height;
    SDL_GetWindowSizeInPixels(sdlWindow->GetSDLWindow(), &width, &height);

    VkExtent2D actualExtent = {static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height)};

    actualExtent.width =
        std::clamp(actualExtent.width,
                   details.caps.minImageExtent.width,
                   details.caps.maxImageExtent.width);

    actualExtent.height =
        std::clamp(actualExtent.height,
                   details.caps.minImageExtent.height,
                   details.caps.maxImageExtent.height);

    return actualExtent;
}

}
}
