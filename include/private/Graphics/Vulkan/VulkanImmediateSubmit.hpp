#ifndef _VULKANIMMEDIATESUBMIT_HPP_
#define _VULKANIMMEDIATESUBMIT_HPP_

#include <vulkan/vulkan.h>

#include <Core/OSDef.hpp>
#include <array>
#include <utility>

namespace Sleak {
namespace RenderEngine {

/// Blocking one-shot submits on the graphics queue for init-time work.
/// Reuses a small set of command buffers and fences instead of creating
/// them per call, and waits on the fence rather than idling the queue.
class ENGINE_API VulkanImmediateSubmit {
   public:
    /// Creates the internal command pool, command buffers and fences.
    static bool Init(VkDevice device, uint32_t queueFamily, VkQueue queue);
    static void Shutdown();
    static bool IsInitialized() { return s_pool != VK_NULL_HANDLE; }

    /// Records through the callback, submits, and waits for the GPU.
    template <typename Fn>
    static bool Run(Fn&& record) {
        VkCommandBuffer cmd = Begin();
        if (cmd == VK_NULL_HANDLE) return false;
        std::forward<Fn>(record)(cmd);
        return EndAndWait(cmd);
    }

    /// Submits an already recorded command buffer and waits for the GPU.
    static bool SubmitAndWait(VkCommandBuffer cmd);

   private:
    static VkCommandBuffer Begin();
    static bool EndAndWait(VkCommandBuffer cmd);
    static bool SubmitSlot(VkCommandBuffer cmd, uint32_t slot);

    static constexpr uint32_t kSlots = 4;

    static VkDevice s_device;
    static VkQueue s_queue;
    static VkCommandPool s_pool;
    static std::array<VkCommandBuffer, kSlots> s_cmds;
    static std::array<VkFence, kSlots> s_fences;
    static uint32_t s_depth;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif
