#ifndef _VULKANPOSTGRAPH_HPP_
#define _VULKANPOSTGRAPH_HPP_

#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <Core/OSDef.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Sleak {
namespace RenderEngine {

/// Small frame graph for the fullscreen post-processing chain. Passes declare
/// the targets they read and the one they write; the graph owns the target
/// images and records the layout transitions and barriers between passes.
class ENGINE_API VulkanPostGraph {
   public:
    using TargetId = uint32_t;
    using PassId = uint32_t;
    static constexpr uint32_t kInvalid = UINT32_MAX;

    /// A render target sized relative to the graph's output extent.
    struct TargetDesc {
        std::string name;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t extentDivisor = 1;
        uint32_t mipLevels = 1;
        VkImageUsageFlags extraUsage = 0;
    };

    /// One mip level of a target.
    struct TargetRef {
        TargetId target = kInvalid;
        uint32_t mip = 0;
    };

    /// A fullscreen pass writing one target mip. `record` binds and draws
    /// inside the render pass the graph has already begun.
    struct PassDesc {
        std::string name;
        uint32_t phase = 0;
        TargetRef output;
        bool loadOutput = false;
        std::vector<TargetRef> reads;
        std::function<void(VkCommandBuffer, VkExtent2D)> record;
    };

    void Init(VkDevice device, VmaAllocator allocator);
    TargetId AddTarget(TargetDesc desc);
    /// Registers a pass. Its render pass exists right away so pipelines can
    /// be created against it before Build.
    PassId AddPass(PassDesc desc);

    /// Allocates every target and framebuffer for the given output extent,
    /// dropping the previous ones first.
    bool Build(VkExtent2D extent);
    /// Frees targets and framebuffers but keeps the declarations.
    void ReleaseTargets();
    /// Destroys everything, including declarations and render passes.
    void Reset();

    /// Records every pass of a phase in declaration order and leaves each
    /// target it wrote shader-readable.
    void Execute(VkCommandBuffer cmd, uint32_t phase);
    /// Clears one target mip and leaves it shader-readable.
    void Clear(VkCommandBuffer cmd, TargetRef ref,
               const VkClearColorValue& color);

    bool IsBuilt() const { return m_built; }
    VkRenderPass GetRenderPass(PassId pass) const;
    VkImage GetImage(TargetId target) const;
    VkImageView GetView(TargetRef ref) const;
    VkExtent2D GetExtent(TargetRef ref) const;

    /// Creates an image with device-local memory from VMA.
    static bool CreateImage(VmaAllocator allocator,
                            const VkImageCreateInfo& info, VkImage& image,
                            VmaAllocation& allocation);

   private:
    struct Target {
        TargetDesc desc;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        std::vector<VkImageView> views;
        std::vector<VkExtent2D> extents;
        std::vector<VkImageLayout> layouts;
    };
    struct Pass {
        PassDesc desc;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };
    struct RenderPassKey {
        VkFormat format;
        bool load;
        VkRenderPass renderPass;
    };

    VkRenderPass GetOrCreateRenderPass(VkFormat format, bool load);
    /// Queues a transition of one target mip into newLayout.
    void Transition(TargetRef ref, VkImageLayout newLayout, bool keepContents,
                    std::vector<VkImageMemoryBarrier>& barriers,
                    VkPipelineStageFlags& srcStages,
                    VkPipelineStageFlags& dstStages);
    static void Flush(VkCommandBuffer cmd,
                      std::vector<VkImageMemoryBarrier>& barriers,
                      VkPipelineStageFlags& srcStages,
                      VkPipelineStageFlags& dstStages);

    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    std::vector<Target> m_targets;
    std::vector<Pass> m_passes;
    std::vector<RenderPassKey> m_renderPasses;
    bool m_built = false;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif
