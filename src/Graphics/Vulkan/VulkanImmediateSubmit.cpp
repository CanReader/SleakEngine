#include "../../include/private/Graphics/Vulkan/VulkanImmediateSubmit.hpp"

#include <Core/Logger.hpp>

namespace Sleak {
namespace RenderEngine {

VkDevice VulkanImmediateSubmit::s_device = VK_NULL_HANDLE;
VkQueue VulkanImmediateSubmit::s_queue = VK_NULL_HANDLE;
VkCommandPool VulkanImmediateSubmit::s_pool = VK_NULL_HANDLE;
std::array<VkCommandBuffer, VulkanImmediateSubmit::kSlots>
    VulkanImmediateSubmit::s_cmds{};
std::array<VkFence, VulkanImmediateSubmit::kSlots>
    VulkanImmediateSubmit::s_fences{};
uint32_t VulkanImmediateSubmit::s_depth = 0;

bool VulkanImmediateSubmit::Init(VkDevice device, uint32_t queueFamily,
                                 VkQueue queue) {
    if (s_pool != VK_NULL_HANDLE) return true;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                     VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &s_pool) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanImmediateSubmit: failed to create command pool");
        s_pool = VK_NULL_HANDLE;
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = s_pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kSlots;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    s_device = device;
    s_queue = queue;
    bool ok = vkAllocateCommandBuffers(device, &allocInfo, s_cmds.data()) ==
              VK_SUCCESS;
    for (uint32_t i = 0; ok && i < kSlots; ++i)
        ok = vkCreateFence(device, &fenceInfo, nullptr, &s_fences[i]) ==
             VK_SUCCESS;
    if (!ok) {
        SLEAK_ERROR("VulkanImmediateSubmit: failed to create submit slots");
        Shutdown();
        return false;
    }
    return true;
}

void VulkanImmediateSubmit::Shutdown() {
    if (s_device == VK_NULL_HANDLE) return;
    for (auto& fence : s_fences) {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(s_device, fence, nullptr);
        fence = VK_NULL_HANDLE;
    }
    if (s_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(s_device, s_pool, nullptr);
    s_cmds = {};
    s_pool = VK_NULL_HANDLE;
    s_queue = VK_NULL_HANDLE;
    s_device = VK_NULL_HANDLE;
    s_depth = 0;
}

VkCommandBuffer VulkanImmediateSubmit::Begin() {
    if (s_pool == VK_NULL_HANDLE) {
        SLEAK_ERROR("VulkanImmediateSubmit: used before Init");
        return VK_NULL_HANDLE;
    }
    if (s_depth >= kSlots) {
        SLEAK_ERROR("VulkanImmediateSubmit: nested too deep");
        return VK_NULL_HANDLE;
    }

    VkCommandBuffer cmd = s_cmds[s_depth];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        SLEAK_ERROR("VulkanImmediateSubmit: failed to begin command buffer");
        return VK_NULL_HANDLE;
    }
    ++s_depth;
    return cmd;
}

bool VulkanImmediateSubmit::EndAndWait(VkCommandBuffer cmd) {
    --s_depth;
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        SLEAK_ERROR("VulkanImmediateSubmit: failed to end command buffer");
        return false;
    }
    return SubmitSlot(cmd, s_depth);
}

bool VulkanImmediateSubmit::SubmitAndWait(VkCommandBuffer cmd) {
    if (s_pool == VK_NULL_HANDLE || s_depth >= kSlots) {
        SLEAK_ERROR("VulkanImmediateSubmit: no free submit slot");
        return false;
    }
    return SubmitSlot(cmd, s_depth);
}

bool VulkanImmediateSubmit::SubmitSlot(VkCommandBuffer cmd, uint32_t slot) {
    VkFence fence = s_fences[slot];
    vkResetFences(s_device, 1, &fence);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    if (vkQueueSubmit(s_queue, 1, &submitInfo, fence) != VK_SUCCESS) {
        SLEAK_ERROR("VulkanImmediateSubmit: queue submit failed");
        return false;
    }
    return vkWaitForFences(s_device, 1, &fence, VK_TRUE, UINT64_MAX) ==
           VK_SUCCESS;
}

}  // namespace RenderEngine
}  // namespace Sleak
