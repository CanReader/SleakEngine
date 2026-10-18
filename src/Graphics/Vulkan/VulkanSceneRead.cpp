#include <Math/Matrix.hpp>
#include <Runtime/VertexLayout.hpp>
#include <algorithm>
#include <array>
#include <cstring>

#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "Core/Logger.hpp"
#include "Graphics/Common/ConstantBuffer.hpp"

namespace Sleak {
namespace RenderEngine {

// ==================================================================
// Scene read for transparent formats (VertexLayoutDesc::transparentReadsScene)
// ==================================================================

namespace {

constexpr uint32_t SINGLE_LAYER = 1;
constexpr uint32_t CUBE_FACE_COUNT = 6;
constexpr VkExtent2D STAND_IN_EXTENT = {1, 1};
constexpr float FAR_DEPTH = 1.0f;
// UV offsets span half the NDC range
constexpr float UV_TO_NDC = 2.0f;

/// True when the depth format carries a stencil aspect.
bool HasStencil(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D16_UNORM_S8_UINT;
}

/// Aspect mask covering every aspect of a depth format.
VkImageAspectFlags DepthAspects(VkFormat format) {
    return VK_IMAGE_ASPECT_DEPTH_BIT |
           (HasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

/// Picks a memory type matching the filter and property flags.
bool FindMemory(VkPhysicalDevice gpu, uint32_t typeBits,
                VkMemoryPropertyFlags flags, uint32_t& index) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) {
            index = i;
            return true;
        }
    }
    return false;
}

/// Creates a device-local 2D image, or a cube when given six layers, and
/// its view.
bool CreateSampledImage(VkDevice device, VkPhysicalDevice gpu, VkFormat format,
                        VkExtent2D extent, uint32_t layers,
                        VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImage& image, VkDeviceMemory& memory,
                        VkImageView& view) {
    const bool cube = layers == CUBE_FACE_COUNT;

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    if (!FindMemory(gpu, req.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    alloc.memoryTypeIndex) ||
        vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(device, image, memory, 0);

    VkImageViewCreateInfo vinfo{};
    vinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vinfo.image = image;
    vinfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vinfo.format = format;
    vinfo.subresourceRange = {aspect, 0, 1, 0, layers};
    return vkCreateImageView(device, &vinfo, nullptr, &view) == VK_SUCCESS;
}

/// Destroys an image, its view, and its memory, nulling the handles.
void DestroyImage(VkDevice device, VkImage& image, VkDeviceMemory& memory,
                  VkImageView& view) {
    if (view) vkDestroyImageView(device, view, nullptr);
    if (image) vkDestroyImage(device, image, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

/// Image barrier over every layer of a single-mip image.
VkImageMemoryBarrier MakeBarrier(VkImage image, VkImageAspectFlags aspect,
                                 uint32_t layers, VkImageLayout from,
                                 VkImageLayout to, VkAccessFlags src,
                                 VkAccessFlags dst) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    return b;
}

/// One image to clear and leave in SHADER_READ_ONLY_OPTIMAL.
struct ClearTarget {
    VkImage image;
    VkImageAspectFlags aspect;
    uint32_t layers;
};

/// Clears images to zero (depth to 1) and leaves them shader readable.
void ClearToShaderRead(VkDevice device, VkCommandPool pool, VkQueue queue,
                       const ClearTarget* targets, uint32_t count) {
    VkCommandBufferAllocateInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandPool = pool;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &ca, &cmd) != VK_SUCCESS) return;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    for (uint32_t i = 0; i < count; ++i) {
        const ClearTarget& t = targets[i];
        const VkImageMemoryBarrier toDst =
            MakeBarrier(t.image, t.aspect, t.layers, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toDst);

        const VkImageSubresourceRange range = {t.aspect, 0, 1, 0, t.layers};
        if (t.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
            const VkClearDepthStencilValue far = {FAR_DEPTH, 0};
            vkCmdClearDepthStencilImage(cmd, t.image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &far, 1, &range);
        } else {
            const VkClearColorValue zero = {};
            vkCmdClearColorImage(cmd, t.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                                 &range);
        }

        const VkImageMemoryBarrier toRead = MakeBarrier(
            t.image, t.aspect, t.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);
    }
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

}  // namespace

/// Creates the scene-read samplers, stand-in images, and per-frame UBOs and
/// writes them into both frames' set 3 so the bindings are always valid.
bool VulkanRenderer::CreateSceneReadResources() {
    if (m_sceneReadCreated) return true;
    if (!m_lightUBOCreated) return false;

    // Samplers
    VkSamplerCreateInfo sinfo{};
    sinfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sinfo.magFilter = VK_FILTER_LINEAR;
    sinfo.minFilter = VK_FILTER_LINEAR;
    sinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &sinfo, nullptr, &m_sceneLinearSampler) !=
        VK_SUCCESS)
        return false;
    sinfo.magFilter = VK_FILTER_NEAREST;
    sinfo.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device, &sinfo, nullptr, &m_scenePointSampler) !=
        VK_SUCCESS)
        return false;

    // Stand-ins
    const VkImageUsageFlags standInUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateSampledImage(device, physicalDevice, VK_FORMAT_R8G8B8A8_UNORM,
                            STAND_IN_EXTENT, SINGLE_LAYER, standInUsage,
                            VK_IMAGE_ASPECT_COLOR_BIT, m_sceneFallbackImage,
                            m_sceneFallbackMemory, m_sceneFallbackView) ||
        !CreateSampledImage(device, physicalDevice, VK_FORMAT_R8G8B8A8_UNORM,
                            STAND_IN_EXTENT, CUBE_FACE_COUNT, standInUsage,
                            VK_IMAGE_ASPECT_COLOR_BIT, m_sceneFallbackCubeImage,
                            m_sceneFallbackCubeMemory,
                            m_sceneFallbackCubeView)) {
        SLEAK_ERROR("VulkanRenderer: scene-read stand-in images failed");
        return false;
    }
    const ClearTarget standIns[] = {
        {m_sceneFallbackImage, VK_IMAGE_ASPECT_COLOR_BIT, SINGLE_LAYER},
        {m_sceneFallbackCubeImage, VK_IMAGE_ASPECT_COLOR_BIT, CUBE_FACE_COUNT}};
    ClearToShaderRead(device, commands, graphicsQueue, standIns,
                      static_cast<uint32_t>(std::size(standIns)));

    // Per-frame UBOs
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo binfo{};
        binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        binfo.size = sizeof(SceneReadUBO);
        binfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &binfo, nullptr,
                           &m_sceneReadUBOBuffers[i]) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, m_sceneReadUBOBuffers[i], &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        if (!FindMemory(physicalDevice, req.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        alloc.memoryTypeIndex) ||
            vkAllocateMemory(device, &alloc, nullptr,
                             &m_sceneReadUBOMemory[i]) != VK_SUCCESS)
            return false;
        vkBindBufferMemory(device, m_sceneReadUBOBuffers[i],
                           m_sceneReadUBOMemory[i], 0);
        vkMapMemory(device, m_sceneReadUBOMemory[i], 0, sizeof(SceneReadUBO), 0,
                    &m_sceneReadUBOMapped[i]);
        std::memset(m_sceneReadUBOMapped[i], 0, sizeof(SceneReadUBO));

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_sceneReadUBOBuffers[i];
        bufInfo.range = sizeof(SceneReadUBO);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_shadowSamplerDescriptorSets[i];
        write.dstBinding = SCENE_UBO_BINDING;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    m_sceneReadCreated = true;
    m_sceneReadWritten = {};
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        UpdateSceneReadDescriptors(i);
    return true;
}

/// Destroys everything CreateSceneReadResources made plus the snapshots.
void VulkanRenderer::CleanupSceneReadResources() {
    CleanupSceneCopyImages();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_sceneReadUBOMapped[i])
            vkUnmapMemory(device, m_sceneReadUBOMemory[i]);
        if (m_sceneReadUBOBuffers[i])
            vkDestroyBuffer(device, m_sceneReadUBOBuffers[i], nullptr);
        if (m_sceneReadUBOMemory[i])
            vkFreeMemory(device, m_sceneReadUBOMemory[i], nullptr);
        m_sceneReadUBOMapped[i] = nullptr;
        m_sceneReadUBOBuffers[i] = VK_NULL_HANDLE;
        m_sceneReadUBOMemory[i] = VK_NULL_HANDLE;
    }
    DestroyImage(device, m_sceneFallbackImage, m_sceneFallbackMemory,
                 m_sceneFallbackView);
    DestroyImage(device, m_sceneFallbackCubeImage, m_sceneFallbackCubeMemory,
                 m_sceneFallbackCubeView);
    if (m_sceneLinearSampler)
        vkDestroySampler(device, m_sceneLinearSampler, nullptr);
    if (m_scenePointSampler)
        vkDestroySampler(device, m_scenePointSampler, nullptr);
    m_sceneLinearSampler = VK_NULL_HANDLE;
    m_scenePointSampler = VK_NULL_HANDLE;
    m_sceneReadWritten = {};
    m_sceneReadCreated = false;
}

/// Creates the extent-sized scene color and depth snapshot images, cleared
/// and shader readable until the first copy lands.
bool VulkanRenderer::CreateSceneCopyImages() {
    CleanupSceneCopyImages();
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateSampledImage(device, physicalDevice, m_hdrSceneFormat, scExtent,
                            SINGLE_LAYER, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                            m_sceneColorCopyImage, m_sceneColorCopyMemory,
                            m_sceneColorCopyView) ||
        !CreateSampledImage(device, physicalDevice, depthFormat, scExtent,
                            SINGLE_LAYER, usage, VK_IMAGE_ASPECT_DEPTH_BIT,
                            m_sceneDepthCopyImage, m_sceneDepthCopyMemory,
                            m_sceneDepthCopyView)) {
        CleanupSceneCopyImages();
        return false;
    }
    const ClearTarget targets[] = {
        {m_sceneColorCopyImage, VK_IMAGE_ASPECT_COLOR_BIT, SINGLE_LAYER},
        {m_sceneDepthCopyImage, DepthAspects(depthFormat), SINGLE_LAYER}};
    ClearToShaderRead(device, commands, graphicsQueue, targets,
                      static_cast<uint32_t>(std::size(targets)));
    return true;
}

/// Destroys the snapshot images and forces a set 3 rewrite on both frames.
void VulkanRenderer::CleanupSceneCopyImages() {
    DestroyImage(device, m_sceneColorCopyImage, m_sceneColorCopyMemory,
                 m_sceneColorCopyView);
    DestroyImage(device, m_sceneDepthCopyImage, m_sceneDepthCopyMemory,
                 m_sceneDepthCopyView);
    m_sceneReadWritten = {};
}

/// Points a frame's set 3 bindings 2-4 at the current snapshot and sky.
/// Call only while that frame's command buffer is not pending.
void VulkanRenderer::UpdateSceneReadDescriptors(uint32_t frame) {
    if (!m_sceneReadCreated) return;

    SceneReadViews want;
    want.color =
        m_sceneColorCopyView ? m_sceneColorCopyView : m_sceneFallbackView;
    want.depth =
        m_sceneDepthCopyView ? m_sceneDepthCopyView : m_sceneFallbackView;
    want.sky =
        m_skyboxCubemapView ? m_skyboxCubemapView : m_sceneFallbackCubeView;
    want.skySerial = m_skyboxViewSerial;

    SceneReadViews& have = m_sceneReadWritten[frame];
    std::array<VkDescriptorImageInfo, SCENE_READ_IMAGE_COUNT> infos{};
    std::array<VkWriteDescriptorSet, SCENE_READ_IMAGE_COUNT> writes{};
    uint32_t count = 0;
    auto add = [&](uint32_t binding, VkImageView view, VkSampler sampler) {
        infos[count].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[count].imageView = view;
        infos[count].sampler = sampler;
        writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[count].dstSet = m_shadowSamplerDescriptorSets[frame];
        writes[count].dstBinding = binding;
        writes[count].descriptorCount = 1;
        writes[count].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[count].pImageInfo = &infos[count];
        ++count;
    };
    if (have.color != want.color)
        add(SCENE_COLOR_BINDING, want.color, m_sceneLinearSampler);
    if (have.depth != want.depth)
        add(SCENE_DEPTH_BINDING, want.depth, m_scenePointSampler);
    if (have.sky != want.sky || have.skySerial != want.skySerial)
        add(SCENE_SKY_BINDING, want.sky,
            m_skyboxCubemapView ? m_skyboxCubemapSampler
                                : m_sceneLinearSampler);
    if (count > 0)
        vkUpdateDescriptorSets(device, count, writes.data(), 0, nullptr);
    have = want;
}

/// Fills this frame's SceneReadUBO and, inside the deferred forward pass,
/// snapshots the scene. Runs once per frame on the first scene-reading draw.
void VulkanRenderer::PrepareSceneRead() {
    if (m_sceneReadPrepared || !m_sceneReadCreated) return;
    m_sceneReadPrepared = true;

    const SceneReadViews& bound = m_sceneReadWritten[currentFrame];
    const bool sceneValid = m_forwardPassOpen && m_inForwardTransparentPass &&
                            m_sceneColorCopyImage != VK_NULL_HANDLE &&
                            bound.color == m_sceneColorCopyView &&
                            bound.depth == m_sceneDepthCopyView;
    if (sceneValid) RecordSceneCopy();

    SceneReadUBO ubo{};
    Math::Matrix4 view, projection;
    std::memcpy(&view(0, 0), m_cachedView, sizeof(m_cachedView));
    std::memcpy(&projection(0, 0), m_cachedProjection,
                sizeof(m_cachedProjection));
    const Math::Matrix4 viewProj = view * projection;
    std::memcpy(ubo.ViewProj, &viewProj(0, 0), sizeof(ubo.ViewProj));
    std::memcpy(ubo.InvViewProj, m_cachedInvViewProj, sizeof(ubo.InvViewProj));
    ubo.ScreenWidth = static_cast<float>(std::max(scExtent.width, 1u));
    ubo.ScreenHeight = static_cast<float>(std::max(scExtent.height, 1u));
    ubo.InvScreenWidth = 1.0f / ubo.ScreenWidth;
    ubo.InvScreenHeight = 1.0f / ubo.ScreenHeight;
    // Same UV-to-NDC scale the GBuffer push constants use
    const bool jitter =
        m_taaEnabled && m_taaResourcesCreated && m_inForwardTransparentPass;
    ubo.JitterX = jitter ? m_taaJitter[0] * UV_TO_NDC : 0.0f;
    ubo.JitterY = jitter ? m_taaJitter[1] * UV_TO_NDC : 0.0f;
    ubo.NearPlane = m_cachedNearPlane;
    ubo.FarPlane = m_cachedFarPlane;
    ubo.SceneValid = static_cast<float>(sceneValid);
    ubo.SkyValid = static_cast<float>(bound.sky != m_sceneFallbackCubeView);
    std::memcpy(m_sceneReadUBOMapped[currentFrame], &ubo, sizeof(ubo));
}

/// Ends the forward pass, copies the HDR scene and depth into the
/// snapshots, and reopens the pass with its contents loaded.
void VulkanRenderer::RecordSceneCopy() {
    vkCmdEndRenderPass(command);

    const VkImageAspectFlags depthAspects = DepthAspects(depthFormat);
    const VkImageMemoryBarrier before[] = {
        MakeBarrier(m_hdrSceneImage, VK_IMAGE_ASPECT_COLOR_BIT, SINGLE_LAYER,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT),
        MakeBarrier(depthImage, depthAspects, SINGLE_LAYER,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT),
        MakeBarrier(m_sceneColorCopyImage, VK_IMAGE_ASPECT_COLOR_BIT,
                    SINGLE_LAYER, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT),
        MakeBarrier(m_sceneDepthCopyImage, depthAspects, SINGLE_LAYER,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT)};
    vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, static_cast<uint32_t>(std::size(before)),
                         before);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {scExtent.width, scExtent.height, 1};
    vkCmdCopyImage(command, m_hdrSceneImage,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_sceneColorCopyImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImage(command, depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_sceneDepthCopyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);

    const VkImageMemoryBarrier after[] = {
        MakeBarrier(m_hdrSceneImage, VK_IMAGE_ASPECT_COLOR_BIT, SINGLE_LAYER,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
        MakeBarrier(depthImage, depthAspects, SINGLE_LAYER,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
        MakeBarrier(m_sceneColorCopyImage, VK_IMAGE_ASPECT_COLOR_BIT,
                    SINGLE_LAYER, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        MakeBarrier(m_sceneDepthCopyImage, depthAspects, SINGLE_LAYER,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(std::size(after)), after);

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = m_forwardRenderPass;
    rpBegin.framebuffer = m_forwardFramebuffers[CurrentFrameIndex];
    rpBegin.renderArea.extent = scExtent;
    vkCmdBeginRenderPass(command, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    FillFullscreenViewportScissor(command, scExtent);
}

}  // namespace RenderEngine
}  // namespace Sleak
