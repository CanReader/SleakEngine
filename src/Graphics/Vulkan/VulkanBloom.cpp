#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include "Core/Logger.hpp"

namespace Sleak {
    namespace RenderEngine {

/// Sets the dynamic viewport and scissor to fill the given extent.
void VulkanRenderer::FillFullscreenViewportScissor(VkCommandBuffer cmd, VkExtent2D ext) {
    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = 0.0f;
    vp.width    = static_cast<float>(ext.width);
    vp.height   = static_cast<float>(ext.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.offset = {0, 0};
    sc.extent = ext;
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

// ==================================================================
// ==================== Bloom (UE4-style pyramid) ===================
// ==================================================================

/// Creates the HDR scene color image, view, and the shared bloom-source sampler.
bool VulkanRenderer::CreateHDRSceneResources() {
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent.width  = scExtent.width;
    info.extent.height = scExtent.height;
    info.extent.depth  = 1;
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.format        = m_hdrSceneFormat;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // TRANSFER_DST_BIT: TAA copies its resolved result back into hdrScene.
    // TRANSFER_SRC_BIT: MSAA resolve from the forward render pass.
    info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    if (!VulkanPostGraph::CreateImage(VulkanBuffer::GetAllocator(), info,
                                      m_hdrSceneImage, m_hdrSceneAllocation))
        return false;

    VkImageViewCreateInfo vinfo{};
    vinfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vinfo.image    = m_hdrSceneImage;
    vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vinfo.format   = m_hdrSceneFormat;
    vinfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vinfo, nullptr, &m_hdrSceneView) != VK_SUCCESS) return false;

    // Shared linear-clamp sampler for every bloom source sample.
    VkSamplerCreateInfo sinfo{};
    sinfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sinfo.magFilter    = VK_FILTER_LINEAR;
    sinfo.minFilter    = VK_FILTER_LINEAR;
    sinfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.minLod       = 0.0f;
    sinfo.maxLod       = 0.0f;
    if (vkCreateSampler(device, &sinfo, nullptr, &m_bloomSampler) != VK_SUCCESS) return false;

    return true;
}

/// Writes a filter-DSL descriptor set for a given source view.
static void WriteSingleImageSampler(VkDevice dev, VkDescriptorSet set,
                                    VkImageView view, VkSampler sampler,
                                    VkImageLayout layout) {
    VkDescriptorImageInfo ii{};
    ii.imageLayout = layout;
    ii.imageView = view;
    ii.sampler = sampler;

    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = 0;
    w.dstArrayElement = 0;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

/// Declares the bloom mip chain target and its threshold, downsample, and
/// upsample passes.
bool VulkanRenderer::CreateBloomTargets() {
    // BLOOM_MIP_COUNT mips starting at half the swapchain size. TRANSFER_DST:
    // when bloom is disabled, mip 0 is cleared to black so the composite pass
    // samples a defined SHADER_READ_ONLY image (no full mip chain).
    VulkanPostGraph::TargetDesc desc;
    desc.name = "bloom";
    desc.format = m_hdrSceneFormat;
    desc.extentDivisor = 2;
    desc.mipLevels = BLOOM_MIP_COUNT;
    desc.extraUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    m_bloomTarget = m_postGraph.AddTarget(desc);

    // Filter set index layout per frame:
    //   [0]              : threshold  (sceneHDR -> bloomMip0)
    //   [1..MIP-1]       : downsamples (bloomMip[i-1] -> bloomMip[i])
    //   [MIP..MIP*2-2]   : upsamples  (bloomMip[MIP-1-k] -> bloomMip[MIP-2-k])
    VulkanPostGraph::PassDesc pass;
    pass.phase = kPostPhaseBloom;
    pass.name = "bloom_threshold";
    pass.output = {m_bloomTarget, 0};
    pass.record = [this](VkCommandBuffer cmd, VkExtent2D) {
        VkDescriptorSet set = m_bloomFilterSets[currentFrame][0];
        WriteSingleImageSampler(device, set, m_hdrSceneView, m_bloomSampler,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_bloomThresholdPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_bloomFilterPipelineLayout, 0, 1, &set, 0,
                                nullptr);
        struct {
            float threshold, knee, p0, p1;
        } thresh{1.0f, 0.5f, 0.0f, 0.0f};
        vkCmdPushConstants(cmd, m_bloomFilterPipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(thresh),
                           &thresh);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    };
    m_bloomThresholdPass = m_postGraph.AddPass(pass);

    for (uint32_t m = 1; m < BLOOM_MIP_COUNT; ++m) {
        pass.name = "bloom_downsample";
        pass.output = {m_bloomTarget, m};
        pass.reads = {{m_bloomTarget, m - 1}};
        pass.record = [this, m](VkCommandBuffer cmd, VkExtent2D) {
            VkDescriptorSet set = m_bloomFilterSets[currentFrame][m];
            const VulkanPostGraph::TargetRef src = {m_bloomTarget, m - 1};
            WriteSingleImageSampler(device, set, m_postGraph.GetView(src),
                                    m_bloomSampler,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_bloomDownsamplePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_bloomFilterPipelineLayout, 0, 1, &set, 0,
                                    nullptr);

            const VkExtent2D srcExt = m_postGraph.GetExtent(src);
            struct {
                float tsx, tsy, karis, p;
            } pc;
            pc.tsx = 1.0f / float(srcExt.width);
            pc.tsy = 1.0f / float(srcExt.height);
            // Karis only on first downsample (firefly kill).
            pc.karis = (m == 1) ? 1.0f : 0.0f;
            pc.p = 0.0f;
            vkCmdPushConstants(cmd, m_bloomFilterPipelineLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                               &pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        };
        m_postGraph.AddPass(pass);
    }

    // Upsample chain, additively blended onto the next larger mip.
    for (uint32_t m = BLOOM_MIP_COUNT - 1; m > 0; --m) {
        pass.name = "bloom_upsample";
        pass.output = {m_bloomTarget, m - 1};
        pass.loadOutput = true;
        pass.reads = {{m_bloomTarget, m}};
        pass.record = [this, m](VkCommandBuffer cmd, VkExtent2D) {
            const uint32_t setIdx = BLOOM_MIP_COUNT + (BLOOM_MIP_COUNT - 1 - m);
            VkDescriptorSet set = m_bloomFilterSets[currentFrame][setIdx];
            const VulkanPostGraph::TargetRef src = {m_bloomTarget, m};
            WriteSingleImageSampler(device, set, m_postGraph.GetView(src),
                                    m_bloomSampler,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_bloomUpsamplePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_bloomFilterPipelineLayout, 0, 1, &set, 0,
                                    nullptr);

            const VkExtent2D srcExt = m_postGraph.GetExtent(src);
            struct {
                float tsx, tsy, radius, intensity;
            } pc;
            pc.tsx = 1.0f / float(srcExt.width);
            pc.tsy = 1.0f / float(srcExt.height);
            pc.radius = 1.0f;
            pc.intensity = 1.0f;
            vkCmdPushConstants(cmd, m_bloomFilterPipelineLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                               &pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        };
        const VulkanPostGraph::PassId id = m_postGraph.AddPass(pass);
        if (m_bloomUpsamplePass == VulkanPostGraph::kInvalid)
            m_bloomUpsamplePass = id;
    }

    return true;
}

/// Creates the swapchain composite render pass.
bool VulkanRenderer::CreateBloomRenderPasses() {
    // Common HDR color attachment.
    auto makeRP = [&](VkAttachmentLoadOp loadOp, VkImageLayout initial,
                      VkFormat fmt, VkImageLayout finalLayout,
                      VkRenderPass& out) -> bool {
        VkAttachmentDescription att{};
        att.format         = fmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = loadOp;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = initial;
        att.finalLayout    = finalLayout;

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sp{};
        sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments    = &ref;

        std::array<VkSubpassDependency, 2> d{};
        d[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        d[0].dstSubpass      = 0;
        d[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        d[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        d[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        d[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        d[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        d[1].srcSubpass      = 0;
        d[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        d[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        d[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        d[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        d[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        d[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo rp{};
        rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 1;
        rp.pAttachments    = &att;
        rp.subpassCount    = 1;
        rp.pSubpasses      = &sp;
        rp.dependencyCount = static_cast<uint32_t>(d.size());
        rp.pDependencies   = d.data();

        return vkCreateRenderPass(device, &rp, nullptr, &out) == VK_SUCCESS;
    };

    // Composite: writes swapchain image, ends in PRESENT_SRC_KHR. ImGui draws here.
    if (!makeRP(VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                scImageFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                m_bloomCompositeRenderPass)) return false;

    return true;
}

/// Creates one composite framebuffer per swapchain image.
bool VulkanRenderer::CreateBloomFramebuffers() {
    // One framebuffer per swapchain image for the composite pass.
    m_bloomCompositeFramebuffers.resize(swapChainImageViews.size());
    for (size_t i = 0; i < swapChainImageViews.size(); ++i) {
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = m_bloomCompositeRenderPass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &swapChainImageViews[i];
        fb.width           = scExtent.width;
        fb.height          = scExtent.height;
        fb.layers          = 1;
        if (vkCreateFramebuffer(device, &fb, nullptr, &m_bloomCompositeFramebuffers[i]) != VK_SUCCESS) return false;
    }

    return true;
}

/// Creates the bloom filter and composite descriptor layouts, pool, and sets.
bool VulkanRenderer::CreateBloomDescriptorResources() {
    // Filter DSL: single combined image sampler (binding 0).
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings    = &b;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_bloomFilterDSL) != VK_SUCCESS) return false;
    }
    // Composite DSL: three combined image samplers (sceneHDR + bloom + SSR).
    {
        std::array<VkDescriptorSetLayoutBinding, 3> binds{};
        for (uint32_t i = 0; i < binds.size(); ++i) {
            binds[i].binding         = i;
            binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(binds.size());
        info.pBindings    = binds.data();
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_bloomCompositeDSL) != VK_SUCCESS) return false;
    }

    // Pool sized for BLOOM_TRANSITION_COUNT filter sets per frame slot + 1 composite set per frame.
    // Composite set now binds 3 samplers (HDR + bloom + SSR).
    VkDescriptorPoolSize sz{};
    sz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sz.descriptorCount = (BLOOM_TRANSITION_COUNT + 3) * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.poolSizeCount = 1;
    pool.pPoolSizes    = &sz;
    pool.maxSets       = (BLOOM_TRANSITION_COUNT + 1) * MAX_FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &m_bloomDescriptorPool) != VK_SUCCESS) return false;

    // Allocate filter sets: MAX_FRAMES_IN_FLIGHT frames × BLOOM_TRANSITION_COUNT transitions.
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        std::array<VkDescriptorSetLayout, BLOOM_TRANSITION_COUNT> layouts;
        layouts.fill(m_bloomFilterDSL);
        VkDescriptorSetAllocateInfo a{};
        a.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        a.descriptorPool     = m_bloomDescriptorPool;
        a.descriptorSetCount = BLOOM_TRANSITION_COUNT;
        a.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(device, &a, m_bloomFilterSets[f].data()) != VK_SUCCESS) return false;
    }

    // Allocate composite sets (one per frame).
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_bloomCompositeDSL);
    VkDescriptorSetAllocateInfo a{};
    a.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    a.descriptorPool     = m_bloomDescriptorPool;
    a.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    a.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(device, &a, m_bloomCompositeSets.data()) != VK_SUCCESS) return false;

    return true;
}

/// Compiles the bloom threshold/downsample/upsample/composite shaders and creates their pipelines.
bool VulkanRenderer::CreateBloomPipelines() {
    // ---- Pipeline layouts ----
    VkPushConstantRange filterPushRange{};
    filterPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    filterPushRange.offset     = 0;
    filterPushRange.size       = 16; // 4 floats, all filter PCs are 16 bytes

    VkPipelineLayoutCreateInfo fliInfo{};
    fliInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    fliInfo.setLayoutCount         = 1;
    fliInfo.pSetLayouts            = &m_bloomFilterDSL;
    fliInfo.pushConstantRangeCount = 1;
    fliInfo.pPushConstantRanges    = &filterPushRange;
    if (vkCreatePipelineLayout(device, &fliInfo, nullptr, &m_bloomFilterPipelineLayout) != VK_SUCCESS) return false;

    VkPushConstantRange compPushRange{};
    compPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    compPushRange.offset     = 0;
    compPushRange.size       = 16;

    VkPipelineLayoutCreateInfo cliInfo{};
    cliInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    cliInfo.setLayoutCount         = 1;
    cliInfo.pSetLayouts            = &m_bloomCompositeDSL;
    cliInfo.pushConstantRangeCount = 1;
    cliInfo.pPushConstantRanges    = &compPushRange;
    if (vkCreatePipelineLayout(device, &cliInfo, nullptr, &m_bloomCompositePipelineLayout) != VK_SUCCESS) return false;

    // ---- Compile shaders ----
    m_bloomThresholdShader = new VulkanShader(device);
    if (!m_bloomThresholdShader->compile("assets/shaders/bloom.vert.spv",
                                         "assets/shaders/bloom_threshold.frag.spv")) return false;
    m_bloomDownsampleShader = new VulkanShader(device);
    if (!m_bloomDownsampleShader->compile("assets/shaders/bloom.vert.spv",
                                          "assets/shaders/bloom_downsample.frag.spv")) return false;
    m_bloomUpsampleShader = new VulkanShader(device);
    if (!m_bloomUpsampleShader->compile("assets/shaders/bloom.vert.spv",
                                        "assets/shaders/bloom_upsample.frag.spv")) return false;
    m_bloomCompositeShader = new VulkanShader(device);
    if (!m_bloomCompositeShader->compile("assets/shaders/bloom.vert.spv",
                                         "assets/shaders/bloom_composite.frag.spv")) return false;

    // ---- Common pipeline state (fullscreen triangle) ----
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    std::vector<VkDynamicState> dynStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    ds.pDynamicStates    = dynStates.data();

    VkPipelineViewportStateCreateInfo vps{};
    vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dss{};
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    // Opaque blend (default).
    VkPipelineColorBlendAttachmentState blendOpaque{};
    blendOpaque.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbOpaque{};
    cbOpaque.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbOpaque.attachmentCount = 1;
    cbOpaque.pAttachments    = &blendOpaque;

    // Additive blend for bloom upsample.
    VkPipelineColorBlendAttachmentState blendAdd{};
    blendAdd.blendEnable         = VK_TRUE;
    blendAdd.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAdd.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAdd.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAdd.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAdd.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAdd.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAdd.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbAdd{};
    cbAdd.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbAdd.attachmentCount = 1;
    cbAdd.pAttachments    = &blendAdd;

    // ---- Threshold pipeline (writes mip 0 of bloom image) ----
    VkPipelineShaderStageCreateInfo threshStages[] = {
        m_bloomThresholdShader->GetVertexInfo(),
        m_bloomThresholdShader->GetFragInfo()
    };
    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount          = 2;
    gpi.pStages             = threshStages;
    gpi.pVertexInputState   = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &dss;
    gpi.pColorBlendState    = &cbOpaque;
    gpi.pDynamicState       = &ds;
    gpi.layout              = m_bloomFilterPipelineLayout;
    gpi.renderPass = m_postGraph.GetRenderPass(m_bloomThresholdPass);
    gpi.subpass             = 0;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_bloomThresholdPipeline) != VK_SUCCESS) return false;

    // ---- Downsample pipeline ----
    VkPipelineShaderStageCreateInfo downStages[] = {
        m_bloomDownsampleShader->GetVertexInfo(),
        m_bloomDownsampleShader->GetFragInfo()
    };
    gpi.pStages = downStages;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_bloomDownsamplePipeline) != VK_SUCCESS) return false;

    // ---- Upsample pipeline (additive blend, LOAD render pass) ----
    VkPipelineShaderStageCreateInfo upStages[] = {
        m_bloomUpsampleShader->GetVertexInfo(),
        m_bloomUpsampleShader->GetFragInfo()
    };
    gpi.pStages          = upStages;
    gpi.pColorBlendState = &cbAdd;
    gpi.renderPass = m_postGraph.GetRenderPass(m_bloomUpsamplePass);
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_bloomUpsamplePipeline) != VK_SUCCESS) return false;

    // ---- Composite pipeline ----
    VkPipelineShaderStageCreateInfo compStages[] = {
        m_bloomCompositeShader->GetVertexInfo(),
        m_bloomCompositeShader->GetFragInfo()
    };
    gpi.pStages          = compStages;
    gpi.pColorBlendState = &cbOpaque;
    gpi.layout           = m_bloomCompositePipelineLayout;
    gpi.renderPass       = m_bloomCompositeRenderPass;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_bloomCompositePipeline) != VK_SUCCESS) return false;

    return true;
}

/// Creates the HDR scene, bloom mip chain, passes, descriptors, pipelines, and TAA resources.
bool VulkanRenderer::CreateBloomResources() {
    if (m_bloomResourcesCreated) return true;

    if (!CreateHDRSceneResources())        { SLEAK_ERROR("Bloom: HDR scene resources failed");  return false; }
    if (!CreateBloomTargets()) {
        SLEAK_ERROR("Bloom: targets failed");
        return false;
    }
    if (!CreateBloomRenderPasses())        { SLEAK_ERROR("Bloom: render passes failed");        return false; }
    if (!CreateBloomFramebuffers())        { SLEAK_ERROR("Bloom: framebuffers failed");         return false; }
    if (!CreateBloomDescriptorResources()) { SLEAK_ERROR("Bloom: descriptor resources failed"); return false; }
    if (!CreateBloomPipelines())           { SLEAK_ERROR("Bloom: pipelines failed");            return false; }
    if (!CreateTAAResources())             { SLEAK_ERROR("Bloom: TAA resources failed");         return false; }

    m_bloomResourcesCreated = true;
    SLEAK_INFO("Bloom + HDR scene resources created ({}x{}, {} mips)",
               scExtent.width, scExtent.height, BLOOM_MIP_COUNT);
    return true;
}

/// Destroys all bloom and HDR scene resources, then cleans up TAA resources.
void VulkanRenderer::CleanupBloomResources() {
    if (!m_bloomResourcesCreated) return;

    if (m_bloomThresholdPipeline)   { vkDestroyPipeline(device, m_bloomThresholdPipeline, nullptr);   m_bloomThresholdPipeline = VK_NULL_HANDLE; }
    if (m_bloomDownsamplePipeline)  { vkDestroyPipeline(device, m_bloomDownsamplePipeline, nullptr);  m_bloomDownsamplePipeline = VK_NULL_HANDLE; }
    if (m_bloomUpsamplePipeline)    { vkDestroyPipeline(device, m_bloomUpsamplePipeline, nullptr);    m_bloomUpsamplePipeline = VK_NULL_HANDLE; }
    if (m_bloomCompositePipeline)   { vkDestroyPipeline(device, m_bloomCompositePipeline, nullptr);   m_bloomCompositePipeline = VK_NULL_HANDLE; }

    if (m_bloomFilterPipelineLayout)    { vkDestroyPipelineLayout(device, m_bloomFilterPipelineLayout, nullptr);    m_bloomFilterPipelineLayout = VK_NULL_HANDLE; }
    if (m_bloomCompositePipelineLayout) { vkDestroyPipelineLayout(device, m_bloomCompositePipelineLayout, nullptr); m_bloomCompositePipelineLayout = VK_NULL_HANDLE; }

    delete m_bloomThresholdShader;  m_bloomThresholdShader  = nullptr;
    delete m_bloomDownsampleShader; m_bloomDownsampleShader = nullptr;
    delete m_bloomUpsampleShader;   m_bloomUpsampleShader   = nullptr;
    delete m_bloomCompositeShader;  m_bloomCompositeShader  = nullptr;

    if (m_bloomDescriptorPool)  { vkDestroyDescriptorPool(device, m_bloomDescriptorPool, nullptr); m_bloomDescriptorPool = VK_NULL_HANDLE; }
    if (m_bloomFilterDSL)       { vkDestroyDescriptorSetLayout(device, m_bloomFilterDSL, nullptr); m_bloomFilterDSL = VK_NULL_HANDLE; }
    if (m_bloomCompositeDSL)    { vkDestroyDescriptorSetLayout(device, m_bloomCompositeDSL, nullptr); m_bloomCompositeDSL = VK_NULL_HANDLE; }

    for (auto& fb : m_bloomCompositeFramebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    m_bloomCompositeFramebuffers.clear();

    if (m_bloomCompositeRenderPass) { vkDestroyRenderPass(device, m_bloomCompositeRenderPass, nullptr); m_bloomCompositeRenderPass = VK_NULL_HANDLE; }

    if (m_hdrSceneView)   { vkDestroyImageView(device, m_hdrSceneView, nullptr); m_hdrSceneView = VK_NULL_HANDLE; }
    if (m_hdrSceneImage) {
        vmaDestroyImage(VulkanBuffer::GetAllocator(), m_hdrSceneImage,
                        m_hdrSceneAllocation);
        m_hdrSceneImage = VK_NULL_HANDLE;
        m_hdrSceneAllocation = VK_NULL_HANDLE;
    }

    if (m_bloomSampler) { vkDestroySampler(device, m_bloomSampler, nullptr); m_bloomSampler = VK_NULL_HANDLE; }

    CleanupTAAResources();
    m_bloomResourcesCreated = false;
}

/// Runs the threshold, downsample, and additive-upsample bloom mip chain, or clears mip 0 when bloom is disabled.
void VulkanRenderer::RenderBloomPass() {
    if (!m_bloomResourcesCreated) return;

    // Bloom disabled: skip the mip chain. The composite still samples mip 0,
    // so keep it black and shader-readable (composite also pushes
    // bloomStrength=0). Static content, so only re-clear after the enabled
    // path has dirtied it.
    if (!m_bloomEnabled) {
        if (m_bloomFallbackPrimed) return;
        const VkClearColorValue black{};
        m_postGraph.Clear(command, {m_bloomTarget, 0}, black);
        m_bloomFallbackPrimed = true;
        return;
    }

    m_bloomFallbackPrimed = false;
    m_postGraph.Execute(command, kPostPhaseBloom);
}

/// Tonemaps and composites the HDR scene, bloom, and SSR into the swapchain image, then draws ImGui.
void VulkanRenderer::RenderBloomCompositePass() {
    if (!m_bloomResourcesCreated) return;

    // Bind sceneHDR (binding 0) + bloomMip[0] (binding 1) + SSR (binding 2).
    // When SSR is disabled/unavailable, fall back to the 1x1 default texture
    // which the shader reads as rgb=0 (multiplied by premultiplied alpha it
    // adds nothing) — safe to leave always bound.
    VkDescriptorSet compSet = m_bloomCompositeSets[currentFrame];
    std::array<VkDescriptorImageInfo, 3> infos{};
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[0].imageView   = m_hdrSceneView;
    infos[0].sampler     = m_bloomSampler;

    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkImageView bloomView = m_postGraph.GetView({m_bloomTarget, 0});
    infos[1].imageView = bloomView;
    infos[1].sampler     = m_bloomSampler;

    // RenderSSRPass() guarantees m_ssrImage ends in SHADER_READ_ONLY_OPTIMAL
    // (either via pipeline output or a cleared-to-black fallback when SSR
    // is disabled). Fall back to the bloom mip view only when SSR resources
    // haven't been created yet (should never happen once init completes).
    infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (m_ssrResourcesCreated && m_ssrView != VK_NULL_HANDLE) {
        infos[2].imageView = m_ssrView;
        infos[2].sampler   = m_ssrSampler ? m_ssrSampler : m_bloomSampler;
    } else {
        infos[2].imageView = bloomView;
        infos[2].sampler   = m_bloomSampler;
    }

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = compSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &infos[i];
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = m_bloomCompositeRenderPass;
    rp.framebuffer       = m_bloomCompositeFramebuffers[CurrentFrameIndex];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = scExtent;
    rp.clearValueCount   = 0;

    vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    FillFullscreenViewportScissor(command, scExtent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomCompositePipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_bloomCompositePipelineLayout, 0, 1, &compSet, 0, nullptr);

    struct { float bloomStrength, exposure, p0, p1; } pc;
    // bloom disabled -> 0 so the (black) mip contributes nothing
    pc.bloomStrength = m_bloomEnabled ? 0.06f : 0.0f;   // UE4-style soft bloom
    pc.exposure      = m_exposure;
    pc.p0            = 0.0f;
    pc.p1            = 0.0f;
    vkCmdPushConstants(command, m_bloomCompositePipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(command, 3, 1, 0, 0);

    // ImGui draws inside the composite pass, AFTER the HDR → LDR tonemap.
    if (bImFrameActive) {
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
    }

    vkCmdEndRenderPass(command);
}

}  // namespace RenderEngine
}  // namespace Sleak
