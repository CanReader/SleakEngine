#include "../../include/private/Graphics/Vulkan/VulkanPostGraph.hpp"

#include <Core/Logger.hpp>
#include <algorithm>

namespace Sleak {
namespace RenderEngine {

void VulkanPostGraph::Init(VkDevice device, VmaAllocator allocator) {
    m_device = device;
    m_allocator = allocator;
}

VulkanPostGraph::TargetId VulkanPostGraph::AddTarget(TargetDesc desc) {
    Target target;
    target.desc = std::move(desc);
    target.desc.extentDivisor = std::max(1u, target.desc.extentDivisor);
    target.desc.mipLevels = std::max(1u, target.desc.mipLevels);
    m_targets.push_back(std::move(target));
    return static_cast<TargetId>(m_targets.size() - 1);
}

VulkanPostGraph::PassId VulkanPostGraph::AddPass(PassDesc desc) {
    Pass pass;
    pass.renderPass = GetOrCreateRenderPass(
        m_targets[desc.output.target].desc.format, desc.loadOutput);
    pass.desc = std::move(desc);
    m_passes.push_back(std::move(pass));
    return static_cast<PassId>(m_passes.size() - 1);
}

bool VulkanPostGraph::CreateImage(VmaAllocator allocator,
                                  const VkImageCreateInfo& info, VkImage& image,
                                  VmaAllocation& allocation) {
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator, &info, &allocInfo, &image, &allocation,
                       nullptr) != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool VulkanPostGraph::Build(VkExtent2D extent) {
    ReleaseTargets();

    for (auto& t : m_targets) {
        const VkExtent2D base = {
            std::max(1u, extent.width / t.desc.extentDivisor),
            std::max(1u, extent.height / t.desc.extentDivisor)};

        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.extent = {base.width, base.height, 1};
        info.mipLevels = t.desc.mipLevels;
        info.arrayLayers = 1;
        info.format = t.desc.format;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT | t.desc.extraUsage;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!CreateImage(m_allocator, info, t.image, t.allocation)) {
            SLEAK_ERROR("PostGraph: failed to allocate target '{}'",
                        t.desc.name);
            return false;
        }

        t.views.assign(t.desc.mipLevels, VK_NULL_HANDLE);
        t.extents.resize(t.desc.mipLevels);
        t.layouts.assign(t.desc.mipLevels, VK_IMAGE_LAYOUT_UNDEFINED);
        for (uint32_t m = 0; m < t.desc.mipLevels; ++m) {
            t.extents[m] = {std::max(1u, base.width >> m),
                            std::max(1u, base.height >> m)};

            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = t.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = t.desc.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
            if (vkCreateImageView(m_device, &view, nullptr, &t.views[m]) !=
                VK_SUCCESS) {
                SLEAK_ERROR("PostGraph: failed to create view for '{}'",
                            t.desc.name);
                return false;
            }
        }
    }

    for (auto& p : m_passes) {
        const TargetRef out = p.desc.output;
        const VkExtent2D ext = GetExtent(out);
        VkImageView view = GetView(out);

        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = p.renderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &view;
        fb.width = ext.width;
        fb.height = ext.height;
        fb.layers = 1;
        if (vkCreateFramebuffer(m_device, &fb, nullptr, &p.framebuffer) !=
            VK_SUCCESS) {
            SLEAK_ERROR("PostGraph: failed to create framebuffer for '{}'",
                        p.desc.name);
            return false;
        }
    }

    m_built = true;
    return true;
}

void VulkanPostGraph::ReleaseTargets() {
    if (m_device == VK_NULL_HANDLE) return;

    for (auto& p : m_passes) {
        if (p.framebuffer)
            vkDestroyFramebuffer(m_device, p.framebuffer, nullptr);
        p.framebuffer = VK_NULL_HANDLE;
    }
    for (auto& t : m_targets) {
        for (VkImageView v : t.views)
            if (v) vkDestroyImageView(m_device, v, nullptr);
        t.views.clear();
        t.extents.clear();
        t.layouts.clear();
        if (t.image) vmaDestroyImage(m_allocator, t.image, t.allocation);
        t.image = VK_NULL_HANDLE;
        t.allocation = VK_NULL_HANDLE;
    }
    m_built = false;
}

void VulkanPostGraph::Reset() {
    ReleaseTargets();
    for (auto& rp : m_renderPasses)
        vkDestroyRenderPass(m_device, rp.renderPass, nullptr);
    m_renderPasses.clear();
    m_passes.clear();
    m_targets.clear();
}

VkRenderPass VulkanPostGraph::GetOrCreateRenderPass(VkFormat format,
                                                    bool load) {
    for (const auto& rp : m_renderPasses)
        if (rp.format == format && rp.load == load) return rp.renderPass;

    // Layout changes happen in the graph's own barriers, so the pass keeps
    // the attachment in COLOR_ATTACHMENT_OPTIMAL on both ends.
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp =
        load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    // Covers inputs the graph does not track, like the HDR scene color the
    // lighting pass just wrote.
    VkSubpassDependency external{};
    external.srcSubpass = VK_SUBPASS_EXTERNAL;
    external.dstSubpass = 0;
    external.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    external.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    external.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    external.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &att;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &external;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(m_device, &info, nullptr, &renderPass) !=
        VK_SUCCESS) {
        SLEAK_ERROR("PostGraph: failed to create render pass");
        return VK_NULL_HANDLE;
    }
    m_renderPasses.push_back({format, load, renderPass});
    return renderPass;
}

void VulkanPostGraph::Transition(TargetRef ref, VkImageLayout newLayout,
                                 bool keepContents,
                                 std::vector<VkImageMemoryBarrier>& barriers,
                                 VkPipelineStageFlags& srcStages,
                                 VkPipelineStageFlags& dstStages) {
    Target& t = m_targets[ref.target];
    VkImageLayout& current = t.layouts[ref.mip];
    if (current == newLayout) return;

    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = keepContents ? current : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, ref.mip, 1, 0, 1};

    switch (current) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            srcStages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            srcStages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        default:
            // Undefined or shader-read: only earlier sampling has to finish.
            srcStages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            b.srcAccessMask = 0;
            break;
    }
    switch (newLayout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            dstStages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            if (keepContents)
                b.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            dstStages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        default:
            dstStages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            break;
    }

    barriers.push_back(b);
    current = newLayout;
}

void VulkanPostGraph::Flush(VkCommandBuffer cmd,
                            std::vector<VkImageMemoryBarrier>& barriers,
                            VkPipelineStageFlags& srcStages,
                            VkPipelineStageFlags& dstStages) {
    if (!barriers.empty()) {
        vkCmdPipelineBarrier(cmd, srcStages, dstStages, 0, 0, nullptr, 0,
                             nullptr, static_cast<uint32_t>(barriers.size()),
                             barriers.data());
    }
    barriers.clear();
    srcStages = 0;
    dstStages = 0;
}

void VulkanPostGraph::Execute(VkCommandBuffer cmd, uint32_t phase) {
    if (!m_built) return;

    std::vector<VkImageMemoryBarrier> barriers;
    VkPipelineStageFlags src = 0, dst = 0;

    for (auto& p : m_passes) {
        if (p.desc.phase != phase) continue;

        for (const TargetRef& r : p.desc.reads)
            Transition(r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true,
                       barriers, src, dst);
        Transition(p.desc.output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   p.desc.loadOutput, barriers, src, dst);
        Flush(cmd, barriers, src, dst);

        const VkExtent2D ext = GetExtent(p.desc.output);
        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = p.renderPass;
        rp.framebuffer = p.framebuffer;
        rp.renderArea = {{0, 0}, ext};
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f,
                      0.0f,
                      static_cast<float>(ext.width),
                      static_cast<float>(ext.height),
                      0.0f,
                      1.0f};
        VkRect2D scissor{{0, 0}, ext};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        p.desc.record(cmd, ext);
        vkCmdEndRenderPass(cmd);
    }

    // Everything outside the graph samples these, so close the phase with
    // all written targets shader-readable.
    for (TargetId id = 0; id < m_targets.size(); ++id) {
        for (uint32_t m = 0; m < m_targets[id].layouts.size(); ++m) {
            if (m_targets[id].layouts[m] ==
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                Transition({id, m}, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           true, barriers, src, dst);
        }
    }
    Flush(cmd, barriers, src, dst);
}

void VulkanPostGraph::Clear(VkCommandBuffer cmd, TargetRef ref,
                            const VkClearColorValue& color) {
    if (!m_built) return;

    std::vector<VkImageMemoryBarrier> barriers;
    VkPipelineStageFlags src = 0, dst = 0;
    Transition(ref, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false, barriers, src,
               dst);
    Flush(cmd, barriers, src, dst);

    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, ref.mip,
                                           1, 0, 1};
    vkCmdClearColorImage(cmd, GetImage(ref.target),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                         &range);

    Transition(ref, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true, barriers,
               src, dst);
    Flush(cmd, barriers, src, dst);
}

VkRenderPass VulkanPostGraph::GetRenderPass(PassId pass) const {
    return pass < m_passes.size() ? m_passes[pass].renderPass : VK_NULL_HANDLE;
}

VkImage VulkanPostGraph::GetImage(TargetId target) const {
    return target < m_targets.size() ? m_targets[target].image : VK_NULL_HANDLE;
}

VkImageView VulkanPostGraph::GetView(TargetRef ref) const {
    if (ref.target >= m_targets.size()) return VK_NULL_HANDLE;
    const auto& views = m_targets[ref.target].views;
    return ref.mip < views.size() ? views[ref.mip] : VK_NULL_HANDLE;
}

VkExtent2D VulkanPostGraph::GetExtent(TargetRef ref) const {
    if (ref.target >= m_targets.size()) return {0, 0};
    const auto& extents = m_targets[ref.target].extents;
    return ref.mip < extents.size() ? extents[ref.mip] : VkExtent2D{0, 0};
}

}  // namespace RenderEngine
}  // namespace Sleak
