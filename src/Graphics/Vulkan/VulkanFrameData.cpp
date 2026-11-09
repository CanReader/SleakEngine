#include <algorithm>
#include <array>
#include <cstring>

#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "Core/Logger.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

// TAA jitter is stored in UV units; NDC spans twice that
constexpr float UV_TO_NDC = 2.0f;

/// Rounds a range size up to the device's uniform buffer offset alignment.
VkDeviceSize AlignUniform(VkDeviceSize size, VkDeviceSize alignment) {
    return alignment > 0 ? (size + alignment - 1) / alignment * alignment
                         : size;
}

}  // namespace

/// Creates one host-visible buffer per frame in flight holding FrameData, the
/// game block, and the reserved cascade and atmosphere blocks, zero-filled.
bool VulkanRenderer::CreateFrameDataResources() {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    const VkDeviceSize alignment = props.limits.minUniformBufferOffsetAlignment;

    m_frameUBORanges[SET2_FRAME_BINDING] = sizeof(FrameData);
    m_frameUBORanges[SET2_USER_BINDING] = FRAME_USER_DATA_MAX_BYTES;
    m_frameUBORanges[SET2_CASCADE_BINDING] = FRAME_CASCADE_BLOCK_BYTES;
    m_frameUBORanges[SET2_ATMOSPHERE_BINDING] = FRAME_ATMOSPHERE_BLOCK_BYTES;
    m_frameUBOSize = 0;
    for (uint32_t b = SET2_FRAME_BINDING; b < SET2_BINDING_COUNT; ++b) {
        m_frameUBOOffsets[b] = m_frameUBOSize;
        m_frameUBOSize += AlignUniform(m_frameUBORanges[b], alignment);
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = m_frameUBOSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufferInfo, nullptr,
                           &m_frameUBOBuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("Failed to create FrameData buffer!");
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, m_frameUBOBuffers[i], &memReqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(
            memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device, &allocInfo, nullptr,
                             &m_frameUBOMemory[i]) != VK_SUCCESS) {
            SLEAK_ERROR("Failed to allocate FrameData memory!");
            return false;
        }
        vkBindBufferMemory(device, m_frameUBOBuffers[i], m_frameUBOMemory[i],
                           0);
        vkMapMemory(device, m_frameUBOMemory[i], 0, m_frameUBOSize, 0,
                    &m_frameUBOMapped[i]);
        std::memset(m_frameUBOMapped[i], 0, m_frameUBOSize);
    }
    return true;
}

/// Points set 2 bindings 1-4 of every frame at that frame's ranges.
void VulkanRenderer::WriteFrameDataDescriptors() {
    constexpr uint32_t FRAME_RANGE_COUNT =
        SET2_BINDING_COUNT - SET2_FRAME_BINDING;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        std::array<VkDescriptorBufferInfo, FRAME_RANGE_COUNT> infos{};
        std::array<VkWriteDescriptorSet, FRAME_RANGE_COUNT> writes{};
        for (uint32_t r = 0; r < FRAME_RANGE_COUNT; ++r) {
            const uint32_t binding = SET2_FRAME_BINDING + r;
            infos[r].buffer = m_frameUBOBuffers[i];
            infos[r].offset = m_frameUBOOffsets[binding];
            infos[r].range = m_frameUBORanges[binding];
            writes[r].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[r].dstSet = m_lightUBODescriptorSets[i];
            writes[r].dstBinding = binding;
            writes[r].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[r].descriptorCount = 1;
            writes[r].pBufferInfo = &infos[r];
        }
        vkUpdateDescriptorSets(device, FRAME_RANGE_COUNT, writes.data(), 0,
                               nullptr);
    }
}

/// Destroys the buffers CreateFrameDataResources made.
void VulkanRenderer::CleanupFrameDataResources() {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_frameUBOMapped[i]) {
            vkUnmapMemory(device, m_frameUBOMemory[i]);
            m_frameUBOMapped[i] = nullptr;
        }
        if (m_frameUBOBuffers[i]) {
            vkDestroyBuffer(device, m_frameUBOBuffers[i], nullptr);
            m_frameUBOBuffers[i] = VK_NULL_HANDLE;
        }
        if (m_frameUBOMemory[i]) {
            vkFreeMemory(device, m_frameUBOMemory[i], nullptr);
            m_frameUBOMemory[i] = VK_NULL_HANDLE;
        }
    }
}

/// Writes this frame's FrameData and game block. The GPU reads them at
/// submit, so the shadow pass recorded in BeginRender sees them too.
void VulkanRenderer::UploadFrameData() {
    if (!bFrameStarted || !m_frameUBOMapped[currentFrame]) return;

    // Jitter and SSAO only run in the GBuffer path
    const bool deferred = IsDeferredEnabled();
    const bool taa = m_taaEnabled && m_taaResourcesCreated && deferred;
    FrameTargetInfo target;
    target.width = scExtent.width;
    target.height = scExtent.height;
    target.jitter[0] = taa ? m_taaJitter[0] * UV_TO_NDC : 0.0f;
    target.jitter[1] = taa ? m_taaJitter[1] * UV_TO_NDC : 0.0f;
    if (m_ssaoEnabled && m_ssaoResourcesCreated && deferred)
        target.featureFlags |= FrameFlagSSAO;
    if (taa) target.featureFlags |= FrameFlagTAA;
    if (m_shadowResourcesCreated && m_shadowPassEnabled)
        target.featureFlags |= FrameFlagShadows;
    if (deferred) target.featureFlags |= FrameFlagDeferred;

    FrameData data{};
    target.output = GetOutputTransform();
    m_frameData.Build(target, data);
    auto* base = static_cast<uint8_t*>(m_frameUBOMapped[currentFrame]);
    std::memcpy(base + m_frameUBOOffsets[SET2_FRAME_BINDING], &data,
                sizeof(data));
    std::memcpy(base + m_frameUBOOffsets[SET2_USER_BINDING],
                m_frameData.GetUserData(), FRAME_USER_DATA_MAX_BYTES);
}

}  // namespace RenderEngine
}  // namespace Sleak
