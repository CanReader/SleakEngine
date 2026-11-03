#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "Core/Logger.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

/// Byte offsets of red, green, and blue inside one 4-byte texel; false when
/// the format is not an 8-bit RGBA or BGRA layout.
bool ChannelOffsets(VkFormat format, uint32_t& r, uint32_t& g, uint32_t& b) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            r = 2;
            g = 1;
            b = 0;
            return true;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            r = 0;
            g = 1;
            b = 2;
            return true;
        default:
            return false;
    }
}

/// Picks a host-visible, host-coherent memory type, preferring cached ones.
uint32_t FindReadbackMemoryType(VkPhysicalDevice gpu, uint32_t typeBits) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (VkMemoryPropertyFlags wanted :
         {coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, coherent}) {
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & wanted) == wanted)
                return i;
        }
    }
    return UINT32_MAX;
}

}  // namespace

/// Arms a UI-free readback of the next presented frame into a binary PPM.
bool VulkanRenderer::RequestFrameCapture(const std::string& path) {
    if (path.empty()) return false;
    if (!m_swapchainTransferSrc) {
        SLEAK_ERROR(
            "Frame capture: swapchain was created without TRANSFER_SRC usage");
        return false;
    }
    uint32_t r, g, b;
    if (!ChannelOffsets(scImageFormat, r, g, b)) {
        SLEAK_ERROR("Frame capture: unsupported swapchain format {}",
                    static_cast<int>(scImageFormat));
        return false;
    }
    m_capturePath = path;
    return true;
}

/// Records the swapchain-image-to-buffer copy for an armed capture. Runs after
/// the final render pass, so the image is in PRESENT_SRC_KHR.
void VulkanRenderer::RecordFrameCaptureCopy() {
    CleanupFrameCapture();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size =
        static_cast<VkDeviceSize>(scExtent.width) * scExtent.height * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &m_captureBuffer) !=
        VK_SUCCESS)
        m_captureBuffer = VK_NULL_HANDLE;
    bool ok = m_captureBuffer != VK_NULL_HANDLE;
    if (ok) {
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, m_captureBuffer, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex =
            FindReadbackMemoryType(physicalDevice, req.memoryTypeBits);
        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(device, &alloc, nullptr, &m_captureMemory) !=
                VK_SUCCESS)
            m_captureMemory = VK_NULL_HANDLE;
        ok = m_captureMemory != VK_NULL_HANDLE &&
             vkBindBufferMemory(device, m_captureBuffer, m_captureMemory, 0) ==
                 VK_SUCCESS;
    }
    if (!ok) {
        SLEAK_ERROR("Frame capture: readback buffer allocation failed");
        CleanupFrameCapture();
        m_capturePath.clear();
        return;
    }

    const VkImage image = swapChainImages[CurrentFrameIndex];

    VkImageMemoryBarrier toCopy{};
    toCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toCopy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toCopy.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.image = image;
    toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toCopy);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {scExtent.width, scExtent.height, 1};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_captureBuffer, 1, &region);

    VkImageMemoryBarrier toPresent = toCopy;
    toPresent.srcAccessMask = 0;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1,
        &toHost, 0, nullptr, 1, &toPresent);

    m_captureExtent = scExtent;
    m_captureFormat = scImageFormat;
    m_captureRecorded = true;
}

/// Waits for the captured frame, writes the PPM, and disarms the capture.
void VulkanRenderer::FinishFrameCapture() {
    m_captureRecorded = false;
    std::string path;
    path.swap(m_capturePath);

    const VkResult wait = vkWaitForFences(
        device, 1, &inFlightFences[currentFrame], VK_TRUE, 10'000'000'000ull);
    void* mapped = nullptr;
    if (wait != VK_SUCCESS ||
        vkMapMemory(device, m_captureMemory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
            VK_SUCCESS) {
        SLEAK_ERROR("Frame capture: readback of {} failed ({})", path,
                    static_cast<int>(wait));
        CleanupFrameCapture();
        return;
    }

    uint32_t r = 0, g = 1, b = 2;
    ChannelOffsets(m_captureFormat, r, g, b);
    const size_t pixels =
        static_cast<size_t>(m_captureExtent.width) * m_captureExtent.height;
    const auto* src = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> rgb(pixels * 3);
    for (size_t i = 0; i < pixels; ++i) {
        rgb[i * 3 + 0] = src[i * 4 + r];
        rgb[i * 3 + 1] = src[i * 4 + g];
        rgb[i * 3 + 2] = src[i * 4 + b];
    }
    vkUnmapMemory(device, m_captureMemory);
    CleanupFrameCapture();

    std::ofstream out(path, std::ios::binary);
    out << "P6\n"
        << m_captureExtent.width << " " << m_captureExtent.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()),
              static_cast<std::streamsize>(rgb.size()));
    if (!out) {
        SLEAK_ERROR("Frame capture: could not write {}", path);
        return;
    }
    SLEAK_INFO("Frame capture: wrote {}x{} to {}", m_captureExtent.width,
               m_captureExtent.height, path);
}

/// Frees the capture readback buffer.
void VulkanRenderer::CleanupFrameCapture() {
    if (m_captureBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_captureBuffer, nullptr);
        m_captureBuffer = VK_NULL_HANDLE;
    }
    if (m_captureMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_captureMemory, nullptr);
        m_captureMemory = VK_NULL_HANDLE;
    }
}

}  // namespace RenderEngine
}  // namespace Sleak
