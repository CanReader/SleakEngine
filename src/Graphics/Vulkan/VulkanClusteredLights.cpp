#include <algorithm>
#include <array>
#include <cstring>

#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "Core/Logger.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

enum ClusterBinding : uint32_t {
    kParams = 0,
    kLights = 1,
    kCells = 2,
    kIndices = 3,
};

constexpr VkDeviceSize kClusterBufferSizes[4] = {
    sizeof(ClusterParamsGPU),
    sizeof(LightGPUEntry) * MAX_CLUSTERED_LIGHTS,
    sizeof(uint32_t) * 2 * CLUSTER_COUNT,
    sizeof(uint32_t) * MAX_CLUSTER_LIGHT_INDICES,
};

}  // namespace

/// Creates the per-frame cluster buffers and the set-4 layout and descriptors.
bool VulkanRenderer::CreateClusterResources() {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t b = 0; b < bindings.size(); ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = b == kParams
                                         ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                         : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    dslInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &m_clusterDSL) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Clustered lights: failed to create descriptor set layout");
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * 3;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_clusterPool) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Clustered lights: failed to create descriptor pool");
        return false;
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_clusterDSL);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_clusterPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &allocInfo, m_clusterSets.data()) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Clustered lights: failed to allocate descriptor sets");
        return false;
    }

    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        auto& frame = m_clusterBuffers[f];
        std::array<VkDescriptorBufferInfo, 4> infos{};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (uint32_t b = 0; b < 4; ++b) {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = kClusterBufferSizes[b];
            bufferInfo.usage = b == kParams
                                   ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                   : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo allocCI{};
            allocCI.usage = VMA_MEMORY_USAGE_AUTO;
            allocCI.flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
            allocCI.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

            VmaAllocationInfo info{};
            if (vmaCreateBuffer(VulkanBuffer::GetAllocator(), &bufferInfo,
                                &allocCI, &frame.buffers[b],
                                &frame.allocations[b], &info) != VK_SUCCESS) {
                SLEAK_ERROR("Clustered lights: failed to create buffer {}", b);
                return false;
            }
            frame.mapped[b] = info.pMappedData;
            std::memset(frame.mapped[b], 0, kClusterBufferSizes[b]);

            infos[b].buffer = frame.buffers[b];
            infos[b].range = kClusterBufferSizes[b];
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = m_clusterSets[f];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = bindings[b].descriptorType;
            writes[b].pBufferInfo = &infos[b];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    SLEAK_INFO("VulkanRenderer: Clustered light buffers created ({}x{}x{})",
               CLUSTER_GRID_X, CLUSTER_GRID_Y, CLUSTER_GRID_Z);
    return true;
}

/// Destroys the cluster buffers, layout, and pool.
void VulkanRenderer::CleanupClusterResources() {
    for (auto& frame : m_clusterBuffers) {
        for (uint32_t b = 0; b < 4; ++b) {
            if (frame.buffers[b])
                vmaDestroyBuffer(VulkanBuffer::GetAllocator(), frame.buffers[b],
                                 frame.allocations[b]);
            frame.buffers[b] = VK_NULL_HANDLE;
            frame.allocations[b] = VK_NULL_HANDLE;
            frame.mapped[b] = nullptr;
        }
    }
    if (m_clusterPool) {
        vkDestroyDescriptorPool(device, m_clusterPool, nullptr);
        m_clusterPool = VK_NULL_HANDLE;
    }
    if (m_clusterDSL) {
        vkDestroyDescriptorSetLayout(device, m_clusterDSL, nullptr);
        m_clusterDSL = VK_NULL_HANDLE;
    }
}

/// Writes the clustered light data into the current frame's buffers. Like the
/// light UBO this runs after BeginRender waited on the frame's fence.
void VulkanRenderer::UpdateClusteredLights(const ClusterParamsGPU& params,
                                           const LightGPUEntry* lights,
                                           const uint32_t* cells,
                                           const uint32_t* indices,
                                           uint32_t indexCount) {
    auto& frame = m_clusterBuffers[currentFrame];
    if (!frame.mapped[kParams]) return;

    const uint32_t lightCount = std::min(
        params.GlobalLightCount + params.LocalLightCount, MAX_CLUSTERED_LIGHTS);
    indexCount = std::min(indexCount, MAX_CLUSTER_LIGHT_INDICES);

    std::memcpy(frame.mapped[kParams], &params, sizeof(params));
    std::memcpy(frame.mapped[kLights], lights,
                lightCount * sizeof(LightGPUEntry));
    std::memcpy(frame.mapped[kCells], cells,
                CLUSTER_COUNT * 2 * sizeof(uint32_t));
    std::memcpy(frame.mapped[kIndices], indices, indexCount * sizeof(uint32_t));
}

}  // namespace RenderEngine
}  // namespace Sleak
