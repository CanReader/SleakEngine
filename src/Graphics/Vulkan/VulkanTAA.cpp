#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanInternal.hpp"

#include <array>
#include <cstring>
#include <vector>
#include "Core/Logger.hpp"

namespace Sleak {
    namespace RenderEngine {

// ==================================================================
// ======================= TAA ======================================
// ==================================================================

/// Creates the ping-pong TAA history images, render pass, framebuffers, descriptors, and pipeline.
bool VulkanRenderer::CreateTAAResources() {
    if (m_taaResourcesCreated) return true;

    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;

    // ---- 1. Two ping-pong history images ----
    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo ic{};
        ic.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ic.imageType     = VK_IMAGE_TYPE_2D;
        ic.extent        = { scExtent.width, scExtent.height, 1 };
        ic.mipLevels     = 1;
        ic.arrayLayers   = 1;
        ic.format        = fmt;
        ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ic.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ic.samples       = VK_SAMPLE_COUNT_1_BIT;
        ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &ic, nullptr, &m_taaImages[i]) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, m_taaImages[i], &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &alloc, nullptr, &m_taaMemory[i]) != VK_SUCCESS) return false;
        vkBindImageMemory(device, m_taaImages[i], m_taaMemory[i], 0);

        VkImageViewCreateInfo vi{};
        vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image            = m_taaImages[i];
        vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vi.format           = fmt;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device, &vi, nullptr, &m_taaViews[i]) != VK_SUCCESS) return false;
    }

    // ---- 2. Initialize both images to SHADER_READ_ONLY (cleared black) ----
    {
        VkCommandBufferAllocateInfo ca{};
        ca.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ca.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandPool        = commands;
        ca.commandBufferCount = 1;
        VkCommandBuffer initCmd;
        vkAllocateCommandBuffers(device, &ca, &initCmd);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(initCmd, &bi);

        VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (int i = 0; i < 2; ++i) {
            VkImageMemoryBarrier bar{};
            bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.srcAccessMask       = 0;
            bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.image               = m_taaImages[i];
            bar.subresourceRange    = sr;
            vkCmdPipelineBarrier(initCmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &bar);
        }
        VkClearColorValue black{};
        for (int i = 0; i < 2; ++i)
            vkCmdClearColorImage(initCmd, m_taaImages[i],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &sr);
        for (int i = 0; i < 2; ++i) {
            VkImageMemoryBarrier bar{};
            bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            bar.image               = m_taaImages[i];
            bar.subresourceRange    = sr;
            vkCmdPipelineBarrier(initCmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &bar);
        }
        vkEndCommandBuffer(initCmd);

        VkSubmitInfo sub{};
        sub.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        sub.commandBufferCount = 1;
        sub.pCommandBuffers    = &initCmd;
        vkQueueSubmit(graphicsQueue, 1, &sub, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue);
        vkFreeCommandBuffers(device, commands, 1, &initCmd);
    }

    // ---- 3. Linear-clamp sampler ----
    {
        VkSamplerCreateInfo ss{};
        ss.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ss.magFilter    = VK_FILTER_LINEAR;
        ss.minFilter    = VK_FILTER_LINEAR;
        ss.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ss.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.minLod = 0.0f; ss.maxLod = 0.0f;
        if (vkCreateSampler(device, &ss, nullptr, &m_taaSampler) != VK_SUCCESS) return false;
    }

    // ---- 4. Render pass (shared for both ping-pong targets) ----
    {
        VkAttachmentDescription att{};
        att.format         = fmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 1;
        rpi.pAttachments    = &att;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sub;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        if (vkCreateRenderPass(device, &rpi, nullptr, &m_taaRenderPass) != VK_SUCCESS) return false;
    }

    // ---- 5. Two framebuffers (one per ping-pong target) ----
    for (int i = 0; i < 2; ++i) {
        VkFramebufferCreateInfo fi{};
        fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = m_taaRenderPass;
        fi.attachmentCount = 1;
        fi.pAttachments    = &m_taaViews[i];
        fi.width           = scExtent.width;
        fi.height          = scExtent.height;
        fi.layers          = 1;
        if (vkCreateFramebuffer(device, &fi, nullptr, &m_taaFramebufs[i]) != VK_SUCCESS) return false;
    }

    // ---- 6. Descriptor set layouts ----
    // set 0: 3 combined image samplers (currentTex, historyTex, gDepth)
    {
        std::array<VkDescriptorSetLayoutBinding, 3> binds{};
        for (uint32_t b = 0; b < 3; ++b) {
            binds[b].binding         = b;
            binds[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[b].descriptorCount = 1;
            binds[b].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = static_cast<uint32_t>(binds.size());
        li.pBindings    = binds.data();
        if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_taaInputDSL) != VK_SUCCESS) return false;
    }
    // set 1: UBO
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings    = &b;
        if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_taaUboDSL) != VK_SUCCESS) return false;
    }

    // ---- 7. Descriptor pool + sets ----
    {
        std::array<VkDescriptorPoolSize, 2> ps{};
        ps[0] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * MAX_FRAMES_IN_FLIGHT };
        ps[1] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1 * MAX_FRAMES_IN_FLIGHT };
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.poolSizeCount = static_cast<uint32_t>(ps.size());
        pi.pPoolSizes    = ps.data();
        pi.maxSets       = 2 * MAX_FRAMES_IN_FLIGHT;
        if (vkCreateDescriptorPool(device, &pi, nullptr, &m_taaPool) != VK_SUCCESS) return false;

        for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = m_taaPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &m_taaInputDSL;
            if (vkAllocateDescriptorSets(device, &ai, &m_taaInputSets[f]) != VK_SUCCESS) return false;

            ai.pSetLayouts = &m_taaUboDSL;
            if (vkAllocateDescriptorSets(device, &ai, &m_taaUboSets[f]) != VK_SUCCESS) return false;
        }
    }

    // ---- 8. UBO buffers (HOST_VISIBLE | HOST_COHERENT) ----
    {
        const VkDeviceSize uboSize = sizeof(TAAParams);
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = uboSize;
        bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
            if (vkCreateBuffer(device, &bi, nullptr, &m_taaUboBuffers[f]) != VK_SUCCESS) return false;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, m_taaUboBuffers[f], &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(device, &alloc, nullptr, &m_taaUboMemory[f]) != VK_SUCCESS) return false;
            vkBindBufferMemory(device, m_taaUboBuffers[f], m_taaUboMemory[f], 0);
            vkMapMemory(device, m_taaUboMemory[f], 0, uboSize, 0, &m_taaUboMapped[f]);
            memset(m_taaUboMapped[f], 0, uboSize);

            // Wire UBO descriptor
            VkDescriptorBufferInfo dbi{ m_taaUboBuffers[f], 0, uboSize };
            VkWriteDescriptorSet wr{};
            wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet          = m_taaUboSets[f];
            wr.dstBinding      = 0;
            wr.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr.descriptorCount = 1;
            wr.pBufferInfo     = &dbi;
            vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
        }
    }

    // ---- 9. Pipeline layout ----
    {
        std::array<VkDescriptorSetLayout, 2> layouts{ m_taaInputDSL, m_taaUboDSL };
        VkPipelineLayoutCreateInfo pli{};
        pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = static_cast<uint32_t>(layouts.size());
        pli.pSetLayouts    = layouts.data();
        if (vkCreatePipelineLayout(device, &pli, nullptr, &m_taaPipelineLayout) != VK_SUCCESS) return false;
    }

    // ---- 10. Pipeline ----
    {
        m_taaShader = new VulkanShader(device);
        if (!m_taaShader->compile("assets/shaders/taa.vert.spv",
                                   "assets/shaders/taa.frag.spv")) {
            SLEAK_ERROR("TAA: failed to compile shaders");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[] = {
            m_taaShader->GetVertexInfo(),
            m_taaShader->GetFragInfo()
        };
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
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

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

        VkPipelineColorBlendAttachmentState blendAtt{};
        blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &blendAtt;

        VkGraphicsPipelineCreateInfo gpi{};
        gpi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpi.stageCount          = 2;
        gpi.pStages             = stages;
        gpi.pVertexInputState   = &vin;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState      = &vps;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState   = &ms;
        gpi.pDepthStencilState  = &dss;
        gpi.pColorBlendState    = &cb;
        gpi.pDynamicState       = &ds;
        gpi.layout              = m_taaPipelineLayout;
        gpi.renderPass          = m_taaRenderPass;
        gpi.subpass             = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr,
                                       &m_taaPipeline) != VK_SUCCESS) {
            SLEAK_ERROR("TAA: failed to create pipeline");
            return false;
        }
    }

    m_taaResourcesCreated = true;
    SLEAK_INFO("TAA resources created ({}x{})", scExtent.width, scExtent.height);
    return true;
}

/// Destroys the TAA pipeline, framebuffers, descriptors, images, and sampler.
void VulkanRenderer::CleanupTAAResources() {
    if (!m_taaResourcesCreated) return;

    vkDeviceWaitIdle(device);

    if (m_taaPipeline)       { vkDestroyPipeline(device, m_taaPipeline, nullptr);             m_taaPipeline = VK_NULL_HANDLE; }
    if (m_taaPipelineLayout) { vkDestroyPipelineLayout(device, m_taaPipelineLayout, nullptr); m_taaPipelineLayout = VK_NULL_HANDLE; }
    delete m_taaShader; m_taaShader = nullptr;

    for (int i = 0; i < 2; ++i) {
        if (m_taaFramebufs[i]) { vkDestroyFramebuffer(device, m_taaFramebufs[i], nullptr); m_taaFramebufs[i] = VK_NULL_HANDLE; }
    }
    if (m_taaRenderPass) { vkDestroyRenderPass(device, m_taaRenderPass, nullptr); m_taaRenderPass = VK_NULL_HANDLE; }

    if (m_taaPool)     { vkDestroyDescriptorPool(device, m_taaPool, nullptr);           m_taaPool = VK_NULL_HANDLE; }
    if (m_taaInputDSL) { vkDestroyDescriptorSetLayout(device, m_taaInputDSL, nullptr);  m_taaInputDSL = VK_NULL_HANDLE; }
    if (m_taaUboDSL)   { vkDestroyDescriptorSetLayout(device, m_taaUboDSL, nullptr);    m_taaUboDSL = VK_NULL_HANDLE; }

    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        if (m_taaUboMapped[f])   { vkUnmapMemory(device, m_taaUboMemory[f]); m_taaUboMapped[f] = nullptr; }
        if (m_taaUboBuffers[f])  { vkDestroyBuffer(device, m_taaUboBuffers[f], nullptr); m_taaUboBuffers[f] = VK_NULL_HANDLE; }
        if (m_taaUboMemory[f])   { vkFreeMemory(device, m_taaUboMemory[f], nullptr);     m_taaUboMemory[f] = VK_NULL_HANDLE; }
    }

    if (m_taaSampler) { vkDestroySampler(device, m_taaSampler, nullptr); m_taaSampler = VK_NULL_HANDLE; }

    for (int i = 0; i < 2; ++i) {
        if (m_taaViews[i])  { vkDestroyImageView(device, m_taaViews[i], nullptr);  m_taaViews[i] = VK_NULL_HANDLE; }
        if (m_taaImages[i]) { vkDestroyImage(device, m_taaImages[i], nullptr);      m_taaImages[i] = VK_NULL_HANDLE; }
        if (m_taaMemory[i]) { vkFreeMemory(device, m_taaMemory[i], nullptr);        m_taaMemory[i] = VK_NULL_HANDLE; }
    }

    m_taaResourcesCreated = false;
}

/// Computes the inverse current view-projection and reprojection blend factor into the TAA UBO.
void VulkanRenderer::UpdateTAAUBO() {
    if (!m_taaResourcesCreated || !m_taaUboMapped[currentFrame]) return;

    // VP = View * Projection (row-major)
    float currentVP[16];
    MatMul4(m_cachedView, m_cachedProjection, currentVP);

    float invCurrentVP[16];
    if (!InvertMat4(currentVP, invCurrentVP)) {
        memset(invCurrentVP, 0, sizeof(invCurrentVP));
        for (int i = 0; i < 4; ++i) invCurrentVP[i * 4 + i] = 1.0f;  // identity fallback
    }

    TAAParams p{};
    memcpy(p.InvCurrentVP, invCurrentVP, sizeof(p.InvCurrentVP));
    memcpy(p.PrevVP,       m_prevViewProj, sizeof(p.PrevVP));
    p.ScreenW     = static_cast<float>(scExtent.width);
    p.ScreenH     = static_cast<float>(scExtent.height);
    // First two frames skip history (prev VP is zero-initialized)
    p.BlendFactor = (m_taaFrameIdx <= 1) ? 1.0f : 0.1f;
    p._pad        = 0.0f;

    memcpy(m_taaUboMapped[currentFrame], &p, sizeof(p));

    // Save current VP as previous for next frame
    memcpy(m_prevViewProj, currentVP, sizeof(m_prevViewProj));
}

/// Resolves the current frame against TAA history and copies the result back into the HDR scene image.
void VulkanRenderer::RenderTAAPass() {
    if (!m_taaResourcesCreated || !m_taaEnabled) return;

    const uint32_t writeIdx = static_cast<uint32_t>(m_taaFrameIdx % 2);
    const uint32_t readIdx  = 1u - writeIdx;

    // ---- 1. Depth barrier: DEPTH_STENCIL_ATTACHMENT → READ_ONLY ----
    // (SSR will skip its own depth barrier since TAA already handles it)
    {
        VkImageMemoryBarrier depBar{};
        depBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depBar.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depBar.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depBar.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depBar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        depBar.image               = depthImage;
        depBar.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &depBar);
    }

    // ---- 2. Transition write target: SHADER_READ_ONLY → COLOR_ATTACHMENT ----
    {
        VkImageMemoryBarrier bar{};
        bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        bar.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.image               = m_taaImages[writeIdx];
        bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    // ---- 3. Update input descriptors for this frame ----
    {
        // binding 0: currentTex = hdrScene (already SHADER_READ_ONLY from forward pass)
        // binding 1: historyTex = taaImages[readIdx] (SHADER_READ_ONLY from init / prev frame)
        // binding 2: gDepth     (now DEPTH_STENCIL_READ_ONLY from step 1)
        VkDescriptorImageInfo infos[3]{};
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = m_hdrSceneView;
        infos[0].sampler     = m_taaSampler;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[1].imageView   = m_taaViews[readIdx];
        infos[1].sampler     = m_taaSampler;
        infos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[2].imageView   = depthImageView;
        infos[2].sampler     = m_taaSampler;

        VkWriteDescriptorSet writes[3]{};
        for (int b = 0; b < 3; ++b) {
            writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet          = m_taaInputSets[currentFrame];
            writes[b].dstBinding      = static_cast<uint32_t>(b);
            writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].descriptorCount = 1;
            writes[b].pImageInfo      = &infos[b];
        }
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    }

    // ---- 4. Update UBO ----
    UpdateTAAUBO();

    // ---- 5. TAA render pass ----
    {
        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_taaRenderPass;
        rp.framebuffer       = m_taaFramebufs[writeIdx];
        rp.renderArea.offset = { 0, 0 };
        rp.renderArea.extent = scExtent;
        rp.clearValueCount   = 0;
        vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
        FillFullscreenViewportScissor(command, scExtent);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipeline);
        VkDescriptorSet sets[2] = { m_taaInputSets[currentFrame], m_taaUboSets[currentFrame] };
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_taaPipelineLayout, 0, 2, sets, 0, nullptr);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        // taaImages[writeIdx] is now SHADER_READ_ONLY_OPTIMAL (render pass finalLayout)
    }

    // ---- 6. Copy TAA resolve → hdrScene ----
    // Both images are SHADER_READ_ONLY; transition for transfer then restore.
    {
        VkImageMemoryBarrier toTransfer[2]{};
        toTransfer[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer[0].oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransfer[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer[0].srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        toTransfer[0].dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer[0].image               = m_taaImages[writeIdx];
        toTransfer[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        toTransfer[1]             = toTransfer[0];
        toTransfer[1].newLayout   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer[1].image       = m_hdrSceneImage;

        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, toTransfer);

        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent         = { scExtent.width, scExtent.height, 1 };
        vkCmdCopyImage(command,
            m_taaImages[writeIdx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_hdrSceneImage,       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &region);

        VkImageMemoryBarrier toRead[2]{};
        toRead[0]              = toTransfer[0];
        toRead[0].oldLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead[0].newLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toRead[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        toRead[1]              = toRead[0];
        toRead[1].oldLayout    = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead[1].image        = m_hdrSceneImage;

        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 2, toRead);
    }

    // Advance ping-pong for next frame
    m_taaFrameIdx++;
}

}  // namespace RenderEngine
}  // namespace Sleak
