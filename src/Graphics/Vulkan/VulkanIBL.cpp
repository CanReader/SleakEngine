#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "../../include/private/Graphics/Common/RenderCommandQueue.hpp"

#include <array>
#include <cstring>
#include "Core/Logger.hpp"

namespace Sleak {
    namespace RenderEngine {

/// Creates stub IBL irradiance, prefilter, and BRDF LUT images, samplers, and descriptor set.
bool VulkanRenderer::CreateIBLResources() {
    if (m_iblResourcesCreated) return true;

    // --- Helper: create a 1×1 black 2D or cube image as a stub ---
    auto createStubImage = [&](uint32_t layers, VkFormat fmt,
                               VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo info{};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.extent        = {1, 1, 1};
        info.mipLevels     = 1;
        info.arrayLayers   = layers;
        info.format        = fmt;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                           | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (layers == 6) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

        if (vkCreateImage(device, &info, nullptr, &img) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, img, &memReqs);
        VkMemoryAllocateInfo memInfo{};
        memInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memInfo.allocationSize  = memReqs.size;
        memInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &memInfo, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(device, img, mem, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                           = img;
        viewInfo.viewType                        = (layers == 6) ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                  : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                          = fmt;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = layers;
        return vkCreateImageView(device, &viewInfo, nullptr, &view) == VK_SUCCESS;
    };

    auto createSampler = [&](bool mips, VkSampler& samp) -> bool {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = mips ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod       = 0.0f;
        si.maxLod       = mips ? static_cast<float>(IBL_PREFILTER_MIP_LEVELS) : 1.0f;
        return vkCreateSampler(device, &si, nullptr, &samp) == VK_SUCCESS;
    };

    constexpr VkFormat hdrFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat lutFmt = VK_FORMAT_R16G16_SFLOAT;

    if (!createStubImage(6, hdrFmt, m_iblIrradianceImage, m_iblIrradianceMemory, m_iblIrradianceView)) {
        SLEAK_ERROR("IBL: Failed to create irradiance image!"); return false;
    }
    if (!createSampler(false, m_iblIrradianceSampler)) {
        SLEAK_ERROR("IBL: Failed to create irradiance sampler!"); return false;
    }

    if (!createStubImage(6, hdrFmt, m_iblPrefilterImage, m_iblPrefilterMemory, m_iblPrefilterView)) {
        SLEAK_ERROR("IBL: Failed to create prefilter image!"); return false;
    }
    if (!createSampler(true, m_iblPrefilterSampler)) {
        SLEAK_ERROR("IBL: Failed to create prefilter sampler!"); return false;
    }

    if (!createStubImage(1, lutFmt, m_iblBrdfLutImage, m_iblBrdfLutMemory, m_iblBrdfLutView)) {
        SLEAK_ERROR("IBL: Failed to create BRDF LUT image!"); return false;
    }
    if (!createSampler(false, m_iblBrdfLutSampler)) {
        SLEAK_ERROR("IBL: Failed to create BRDF LUT sampler!"); return false;
    }

    // Transition stub images to SHADER_READ_ONLY_OPTIMAL so they can be sampled
    {
        VkCommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAlloc.commandPool        = commands;
        cmdAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        auto transitionImage = [&](VkImage img, uint32_t layers) {
            VkImageMemoryBarrier bar{};
            bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image               = img;
            bar.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
            bar.srcAccessMask       = 0;
            bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &bar);
        };
        transitionImage(m_iblIrradianceImage, 6);
        transitionImage(m_iblPrefilterImage,  6);
        transitionImage(m_iblBrdfLutImage,    1);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence;
        vkCreateFence(device, &fenceInfo, nullptr, &fence);
        vkQueueSubmit(graphicsQueue, 1, &submit, fence);
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, commands, 1, &cmd);
    }

    // --- Descriptor Set Layout: 3 samplerCubes + 1 sampler2D + 1 UBO ---
    std::array<VkDescriptorSetLayoutBinding, 4> iblBindings{};
    // binding 0: irradiance cubemap
    iblBindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // binding 1: prefilter cubemap
    iblBindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // binding 2: BRDF LUT
    iblBindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // binding 3: IBLSettings UBO
    iblBindings[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo iblDSLInfo{};
    iblDSLInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    iblDSLInfo.bindingCount = static_cast<uint32_t>(iblBindings.size());
    iblDSLInfo.pBindings    = iblBindings.data();
    if (vkCreateDescriptorSetLayout(device, &iblDSLInfo, nullptr, &m_iblDSL) != VK_SUCCESS) {
        SLEAK_ERROR("IBL: Failed to create IBL DSL!"); return false;
    }

    // --- IBL Descriptor Pool ---
    std::array<VkDescriptorPoolSize, 2> iblPoolSizes{};
    iblPoolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * MAX_FRAMES_IN_FLIGHT};
    iblPoolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo iblPoolInfo{};
    iblPoolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    iblPoolInfo.poolSizeCount = static_cast<uint32_t>(iblPoolSizes.size());
    iblPoolInfo.pPoolSizes    = iblPoolSizes.data();
    iblPoolInfo.maxSets       = MAX_FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(device, &iblPoolInfo, nullptr, &m_iblPool) != VK_SUCCESS) {
        SLEAK_ERROR("IBL: Failed to create IBL pool!"); return false;
    }

    // --- Allocate descriptor sets ---
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> iblLayouts;
    iblLayouts.fill(m_iblDSL);
    VkDescriptorSetAllocateInfo iblAlloc{};
    iblAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    iblAlloc.descriptorPool     = m_iblPool;
    iblAlloc.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    iblAlloc.pSetLayouts        = iblLayouts.data();
    if (vkAllocateDescriptorSets(device, &iblAlloc, m_iblSets.data()) != VK_SUCCESS) {
        SLEAK_ERROR("IBL: Failed to allocate IBL descriptor sets!"); return false;
    }

    // --- Per-frame IBL settings UBO (IBLSettingsGPUData, 16 bytes) ---
    constexpr VkDeviceSize settingsSize = sizeof(IBLSettingsGPUData);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size        = settingsSize;
        bufInfo.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufInfo, nullptr, &m_iblSettingsBuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("IBL: Failed to create settings buffer {}!", i); return false;
        }
        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, m_iblSettingsBuffers[i], &memReqs);
        VkMemoryAllocateInfo memInfo{};
        memInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memInfo.allocationSize  = memReqs.size;
        memInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device, &memInfo, nullptr, &m_iblSettingsMemory[i]) != VK_SUCCESS) {
            SLEAK_ERROR("IBL: Failed to allocate settings memory {}!", i); return false;
        }
        vkBindBufferMemory(device, m_iblSettingsBuffers[i], m_iblSettingsMemory[i], 0);
        vkMapMemory(device, m_iblSettingsMemory[i], 0, settingsSize, 0, &m_iblSettingsMapped[i]);

        // Default: IBL disabled — lighting shader falls back to hemisphere ambient
        IBLSettingsGPUData settings{};
        settings.IBLEnabled       = 0;
        settings.IBLIntensity     = 1.0f;
        settings.MaxReflectionLOD = static_cast<float>(IBL_PREFILTER_MIP_LEVELS - 1);
        memcpy(m_iblSettingsMapped[i], &settings, sizeof(settings));
    }

    // --- Write initial descriptor sets ---
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorImageInfo irradianceInfo{};
        irradianceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        irradianceInfo.imageView   = m_iblIrradianceView;
        irradianceInfo.sampler     = m_iblIrradianceSampler;

        VkDescriptorImageInfo prefilterInfo{};
        prefilterInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        prefilterInfo.imageView   = m_iblPrefilterView;
        prefilterInfo.sampler     = m_iblPrefilterSampler;

        VkDescriptorImageInfo lutInfo{};
        lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        lutInfo.imageView   = m_iblBrdfLutView;
        lutInfo.sampler     = m_iblBrdfLutSampler;

        VkDescriptorBufferInfo settingsBufInfo{};
        settingsBufInfo.buffer = m_iblSettingsBuffers[i];
        settingsBufInfo.offset = 0;
        settingsBufInfo.range  = sizeof(IBLSettingsGPUData);

        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     m_iblSets[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     &irradianceInfo, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     m_iblSets[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     &prefilterInfo, nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     m_iblSets[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     &lutInfo, nullptr, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     m_iblSets[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     nullptr, &settingsBufInfo, nullptr};
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    m_iblResourcesCreated = true;
    m_iblReady            = false;  // not yet precomputed
    SLEAK_INFO("VulkanRenderer: IBL resources created (IBL disabled until skybox precompute)");
    return true;
}

/// Destroys the IBL images, samplers, and descriptor resources.
void VulkanRenderer::CleanupIBLResources() {
    if (!m_iblResourcesCreated) return;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_iblSettingsMapped[i]) {
            vkUnmapMemory(device, m_iblSettingsMemory[i]);
            m_iblSettingsMapped[i] = nullptr;
        }
        if (m_iblSettingsBuffers[i]) {
            vkDestroyBuffer(device, m_iblSettingsBuffers[i], nullptr);
            m_iblSettingsBuffers[i] = VK_NULL_HANDLE;
        }
        if (m_iblSettingsMemory[i]) {
            vkFreeMemory(device, m_iblSettingsMemory[i], nullptr);
            m_iblSettingsMemory[i] = VK_NULL_HANDLE;
        }
    }

    if (m_iblPool) { vkDestroyDescriptorPool(device, m_iblPool, nullptr); m_iblPool = VK_NULL_HANDLE; }
    if (m_iblDSL)  { vkDestroyDescriptorSetLayout(device, m_iblDSL, nullptr); m_iblDSL = VK_NULL_HANDLE; }

    if (m_iblBrdfLutSampler)  { vkDestroySampler(device, m_iblBrdfLutSampler, nullptr);  m_iblBrdfLutSampler  = VK_NULL_HANDLE; }
    if (m_iblBrdfLutView)     { vkDestroyImageView(device, m_iblBrdfLutView, nullptr);   m_iblBrdfLutView     = VK_NULL_HANDLE; }
    if (m_iblBrdfLutImage)    { vkDestroyImage(device, m_iblBrdfLutImage, nullptr);      m_iblBrdfLutImage    = VK_NULL_HANDLE; }
    if (m_iblBrdfLutMemory)   { vkFreeMemory(device, m_iblBrdfLutMemory, nullptr);       m_iblBrdfLutMemory   = VK_NULL_HANDLE; }

    if (m_iblPrefilterSampler)  { vkDestroySampler(device, m_iblPrefilterSampler, nullptr);  m_iblPrefilterSampler  = VK_NULL_HANDLE; }
    if (m_iblPrefilterView)     { vkDestroyImageView(device, m_iblPrefilterView, nullptr);   m_iblPrefilterView     = VK_NULL_HANDLE; }
    if (m_iblPrefilterImage)    { vkDestroyImage(device, m_iblPrefilterImage, nullptr);      m_iblPrefilterImage    = VK_NULL_HANDLE; }
    if (m_iblPrefilterMemory)   { vkFreeMemory(device, m_iblPrefilterMemory, nullptr);       m_iblPrefilterMemory   = VK_NULL_HANDLE; }

    if (m_iblIrradianceSampler)  { vkDestroySampler(device, m_iblIrradianceSampler, nullptr);  m_iblIrradianceSampler  = VK_NULL_HANDLE; }
    if (m_iblIrradianceView)     { vkDestroyImageView(device, m_iblIrradianceView, nullptr);   m_iblIrradianceView     = VK_NULL_HANDLE; }
    if (m_iblIrradianceImage)    { vkDestroyImage(device, m_iblIrradianceImage, nullptr);      m_iblIrradianceImage    = VK_NULL_HANDLE; }
    if (m_iblIrradianceMemory)   { vkFreeMemory(device, m_iblIrradianceMemory, nullptr);       m_iblIrradianceMemory   = VK_NULL_HANDLE; }

    m_iblResourcesCreated = false;
    m_iblReady            = false;
}

}  // namespace RenderEngine
}  // namespace Sleak
