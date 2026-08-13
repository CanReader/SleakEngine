#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"

#include <Camera/Camera.hpp>
#include <array>
#include <cstring>
#include <vector>
#include "Core/Logger.hpp"

namespace Sleak {
    namespace RenderEngine {

// ==================================================================
// ==================== SSR (Screen-Space Reflections) ==============
// ==================================================================
// Full-resolution view-space ray march with binary search refinement.
// Runs AFTER the forward pass (HDR scene must be lit and in
// SHADER_READ_ONLY_OPTIMAL) and BEFORE the bloom threshold pass — the
// composite pass then additively blends the SSR result into the HDR scene.

/// Creates the SSR image, render pass, framebuffer, descriptors, UBOs, and pipeline.
bool VulkanRenderer::CreateSSRResources() {
    if (m_ssrResourcesCreated) return true;

    // ---- 1. SSR image (full-res, R16G16B16A16 premultiplied) ----
    {
        VkImageCreateInfo ic{};
        ic.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ic.imageType     = VK_IMAGE_TYPE_2D;
        ic.extent.width  = scExtent.width;
        ic.extent.height = scExtent.height;
        ic.extent.depth  = 1;
        ic.mipLevels     = 1;
        ic.arrayLayers   = 1;
        ic.format        = m_ssrFormat;
        ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // TRANSFER_DST enables vkCmdClearColorImage when SSR is disabled
        // (the composite pass always samples this buffer regardless).
        ic.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ic.samples       = VK_SAMPLE_COUNT_1_BIT;
        ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &ic, nullptr, &m_ssrImage) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateImage failed"); return false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, m_ssrImage, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &alloc, nullptr, &m_ssrMemory) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkAllocateMemory failed ({}B, typeIdx={})", req.size, alloc.memoryTypeIndex); return false;
        }
        vkBindImageMemory(device, m_ssrImage, m_ssrMemory, 0);

        VkImageViewCreateInfo vi{};
        vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image    = m_ssrImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = m_ssrFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &m_ssrView) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateImageView failed"); return false;
        }
    }

    // Linear-clamp sampler — bloom composite samples the SSR buffer.
    {
        VkSamplerCreateInfo ss{};
        ss.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ss.magFilter    = VK_FILTER_LINEAR;
        ss.minFilter    = VK_FILTER_LINEAR;
        ss.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ss.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.minLod       = 0.0f;
        ss.maxLod       = 0.0f;
        if (vkCreateSampler(device, &ss, nullptr, &m_ssrSampler) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateSampler failed"); return false;
        }
    }

    // ---- 2. Render pass (single color attachment) ----
    {
        VkAttachmentDescription att{};
        att.format         = m_ssrFormat;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // we write every pixel
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sp{};
        sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments    = &ref;

        // External dependencies mirror the SSAO render pass — we read from
        // GBuffer/HDR samplers before the pass and the composite samples us
        // after, so we bracket with shader-read-to-color-write / color-write-
        // to-shader-read transitions.
        std::array<VkSubpassDependency, 2> deps{};
        deps[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass      = 0;
        deps[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        deps[1].srcSubpass      = 0;
        deps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo rp{};
        rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 1;
        rp.pAttachments    = &att;
        rp.subpassCount    = 1;
        rp.pSubpasses      = &sp;
        rp.dependencyCount = static_cast<uint32_t>(deps.size());
        rp.pDependencies   = deps.data();
        if (vkCreateRenderPass(device, &rp, nullptr, &m_ssrRenderPass) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateRenderPass failed"); return false;
        }
    }

    // ---- 3. Framebuffer (single view) ----
    {
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = m_ssrRenderPass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &m_ssrView;
        fb.width           = scExtent.width;
        fb.height          = scExtent.height;
        fb.layers          = 1;
        if (vkCreateFramebuffer(device, &fb, nullptr, &m_ssrFramebuffer) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateFramebuffer failed"); return false;
        }
    }

    // ---- 4. Descriptor set layouts ----
    // Set 0: 5 combined image samplers (gNormalRough, gDepth, gMetalEmit,
    //         gAlbedoAO, sceneHDR). World position reconstructed from depth.
    {
        std::array<VkDescriptorSetLayoutBinding, 5> binds{};
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
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_ssrInputDSL) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateDescriptorSetLayout (input) failed"); return false;
        }
    }
    // Set 1: UBO.
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings    = &b;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_ssrUboDSL) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateDescriptorSetLayout (ubo) failed"); return false;
        }
    }

    // ---- 5. Descriptor pool + sets ----
    {
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = 6 * MAX_FRAMES_IN_FLIGHT;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[1].descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo pool{};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool.pPoolSizes    = sizes.data();
        pool.maxSets       = 2 * MAX_FRAMES_IN_FLIGHT;
        if (vkCreateDescriptorPool(device, &pool, nullptr, &m_ssrPool) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateDescriptorPool failed"); return false;
        }

        auto allocSets = [&](VkDescriptorSetLayout dsl, std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>& out) -> bool {
            std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
            layouts.fill(dsl);
            VkDescriptorSetAllocateInfo a{};
            a.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            a.descriptorPool     = m_ssrPool;
            a.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
            a.pSetLayouts        = layouts.data();
            return vkAllocateDescriptorSets(device, &a, out.data()) == VK_SUCCESS;
        };
        if (!allocSets(m_ssrInputDSL, m_ssrInputSets)) {
            SLEAK_ERROR("SSR: vkAllocateDescriptorSets (input) failed"); return false;
        }
        if (!allocSets(m_ssrUboDSL, m_ssrUboSets)) {
            SLEAK_ERROR("SSR: vkAllocateDescriptorSets (ubo) failed"); return false;
        }
    }

    // ---- 6. UBO buffers (host visible coherent) ----
    {
        static constexpr VkDeviceSize uboSize = sizeof(SSRParams);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VkBufferCreateInfo bi{};
            bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size        = uboSize;
            bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bi, nullptr, &m_ssrUboBuffers[i]) != VK_SUCCESS) {
                SLEAK_ERROR("SSR: vkCreateBuffer UBO[{}] failed", i); return false;
            }
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, m_ssrUboBuffers[i], &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize  = req.size;
            alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(device, &alloc, nullptr, &m_ssrUboMemory[i]) != VK_SUCCESS) {
                SLEAK_ERROR("SSR: vkAllocateMemory UBO[{}] failed", i); return false;
            }
            vkBindBufferMemory(device, m_ssrUboBuffers[i], m_ssrUboMemory[i], 0);
            if (vkMapMemory(device, m_ssrUboMemory[i], 0, uboSize, 0, &m_ssrUboMapped[i]) != VK_SUCCESS) return false;

            // Bind UBO to set 1 immediately — input descriptors are written
            // later by UpdateSSRDescriptors() once all source views exist.
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = m_ssrUboBuffers[i];
            bufInfo.offset = 0;
            bufInfo.range  = uboSize;

            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = m_ssrUboSets[i];
            w.dstBinding      = 0;
            w.dstArrayElement = 0;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.descriptorCount = 1;
            w.pBufferInfo     = &bufInfo;
            vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        }
    }

    // ---- 7. Pipeline layout ----
    {
        std::array<VkDescriptorSetLayout, 2> layouts = { m_ssrInputDSL, m_ssrUboDSL };
        VkPipelineLayoutCreateInfo pli{};
        pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = static_cast<uint32_t>(layouts.size());
        pli.pSetLayouts    = layouts.data();
        if (vkCreatePipelineLayout(device, &pli, nullptr, &m_ssrPipelineLayout) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreatePipelineLayout failed"); return false;
        }
    }

    // ---- 8. Pipeline ----
    {
        m_ssrShader = new VulkanShader(device);
        if (!m_ssrShader->compile("assets/shaders/ssr.vert.spv",
                                   "assets/shaders/ssr.frag.spv")) {
            SLEAK_ERROR("SSR: failed to compile ssr shaders");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[] = {
            m_ssrShader->GetVertexInfo(),
            m_ssrShader->GetFragInfo()
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
        // no depth test / write — full-screen post-process

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
        gpi.layout              = m_ssrPipelineLayout;
        gpi.renderPass          = m_ssrRenderPass;
        gpi.subpass             = 0;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_ssrPipeline) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: failed to create pipeline");
            return false;
        }
    }

    m_ssrResourcesCreated = true;
    SLEAK_INFO("SSR resources created ({}x{})", scExtent.width, scExtent.height);
    return true;
}

/// Destroys the SSR pipeline, framebuffer, descriptors, image, and sampler.
void VulkanRenderer::CleanupSSRResources() {
    if (!m_ssrResourcesCreated) return;

    if (m_ssrPipeline)        { vkDestroyPipeline(device, m_ssrPipeline, nullptr);             m_ssrPipeline = VK_NULL_HANDLE; }
    if (m_ssrPipelineLayout)  { vkDestroyPipelineLayout(device, m_ssrPipelineLayout, nullptr); m_ssrPipelineLayout = VK_NULL_HANDLE; }
    delete m_ssrShader; m_ssrShader = nullptr;

    if (m_ssrFramebuffer)     { vkDestroyFramebuffer(device, m_ssrFramebuffer, nullptr);       m_ssrFramebuffer = VK_NULL_HANDLE; }
    if (m_ssrRenderPass)      { vkDestroyRenderPass(device, m_ssrRenderPass, nullptr);         m_ssrRenderPass = VK_NULL_HANDLE; }

    if (m_ssrPool)            { vkDestroyDescriptorPool(device, m_ssrPool, nullptr);           m_ssrPool = VK_NULL_HANDLE; }
    if (m_ssrInputDSL)        { vkDestroyDescriptorSetLayout(device, m_ssrInputDSL, nullptr);  m_ssrInputDSL = VK_NULL_HANDLE; }
    if (m_ssrUboDSL)          { vkDestroyDescriptorSetLayout(device, m_ssrUboDSL, nullptr);    m_ssrUboDSL = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_ssrUboMapped[i])  { vkUnmapMemory(device, m_ssrUboMemory[i]); m_ssrUboMapped[i] = nullptr; }
        if (m_ssrUboBuffers[i]) { vkDestroyBuffer(device, m_ssrUboBuffers[i], nullptr); m_ssrUboBuffers[i] = VK_NULL_HANDLE; }
        if (m_ssrUboMemory[i])  { vkFreeMemory(device, m_ssrUboMemory[i], nullptr);     m_ssrUboMemory[i] = VK_NULL_HANDLE; }
    }

    if (m_ssrView)     { vkDestroyImageView(device, m_ssrView, nullptr);  m_ssrView = VK_NULL_HANDLE; }
    if (m_ssrImage)    { vkDestroyImage(device, m_ssrImage, nullptr);     m_ssrImage = VK_NULL_HANDLE; }
    if (m_ssrMemory)   { vkFreeMemory(device, m_ssrMemory, nullptr);      m_ssrMemory = VK_NULL_HANDLE; }
    if (m_ssrSampler)  { vkDestroySampler(device, m_ssrSampler, nullptr); m_ssrSampler = VK_NULL_HANDLE; }

    m_ssrResourcesCreated = false;
}

// ---- Update / render half (physically separated in the original monolith) ----

/// Writes the GBuffer and HDR scene samplers into the SSR input descriptor sets.
void VulkanRenderer::UpdateSSRDescriptors() {
    if (!m_ssrResourcesCreated) return;

    // Write the input samplers for every frame slot. Called during init, so
    // no concurrent GPU access — safe to update both slots at once.
    // Sampler slots: [0]=gNormalRough, [1]=gDepth, [2]=gMetalEmit,
    //                [3]=gAlbedoAO, [4]=sceneHDR. World pos from depth.
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        std::array<VkDescriptorImageInfo, 5> infos{};

        // gNormalRough = gbuffer[1]
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = m_gbufferViews[1];
        infos[0].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // gDepth — READ_ONLY because shadow pass finalLayout is
        // DEPTH_STENCIL_READ_ONLY, GBuffer pass final is READ_ONLY, but the
        // forward pass exits at DEPTH_STENCIL_ATTACHMENT_OPTIMAL. SSR bracket
        // transitions it to READ_ONLY before sampling (see RenderSSRPass).
        infos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[1].imageView   = depthImageView;
        infos[1].sampler     = m_depthSampler ? m_depthSampler : m_ssrSampler;

        // gMetalEmit = gbuffer[2]
        infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[2].imageView   = m_gbufferViews[2];
        infos[2].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // gAlbedoAO = gbuffer[0]
        infos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[3].imageView   = m_gbufferViews[0];
        infos[3].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // sceneHDR — forward pass finalLayout is SHADER_READ_ONLY_OPTIMAL.
        infos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[4].imageView   = m_hdrSceneView;
        infos[4].sampler     = m_ssrSampler;

        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = m_ssrInputSets[f];
            writes[i].dstBinding      = i;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

/// Fills the SSR UBO with the cached camera matrices, camera position, and ray march parameters.
void VulkanRenderer::UpdateSSRUBO() {
    if (!m_ssrResourcesCreated || !m_ssrUboMapped[currentFrame]) return;

    SSRParams p{};
    memcpy(p.View,        m_cachedView,        sizeof(p.View));
    memcpy(p.Projection,  m_cachedProjection,  sizeof(p.Projection));
    memcpy(p.InvViewProj, m_cachedInvViewProj, sizeof(p.InvViewProj));

    const auto& camPos = Camera::GetMainCameraPosition();
    p.CameraPos[0] = camPos.GetX();
    p.CameraPos[1] = camPos.GetY();
    p.CameraPos[2] = camPos.GetZ();
    p.CameraPos[3] = 0.0f;

    p.ScreenW = static_cast<float>(scExtent.width);
    p.ScreenH = static_cast<float>(scExtent.height);

    // Quality vs perf defaults — 32 coarse + 8 binary is the sweet spot
    // for full-res UE-style SSR. Thickness in view-space *depth* units
    // (post-divide), 0.02 catches near+mid hits without smearing through
    // thin geometry.
    p.MaxDistance        = 20.0f;
    p.Thickness          = 0.5f;
    p.NumSteps           = 20;
    p.NumBinarySteps     = 6;
    p.RoughnessThreshold = 0.9f;
    p._pad               = 0.0f;

    memcpy(m_ssrUboMapped[currentFrame], &p, sizeof(p));
}

/// Ray marches screen-space reflections into the SSR buffer, or clears it when SSR is disabled.
void VulkanRenderer::RenderSSRPass() {
    if (!m_ssrResourcesCreated) return;

    // Depth must be in DEPTH_STENCIL_READ_ONLY_OPTIMAL before SSR samples it.
    // When TAA is enabled (and ran before SSR), it already issued this barrier.
    // When TAA is disabled, do it here instead.
    if (!m_taaResourcesCreated || !m_taaEnabled) {
        VkImageMemoryBarrier depthBarrier{};
        depthBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthBarrier.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthBarrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        depthBarrier.image               = depthImage;
        depthBarrier.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
    }

    // When SSR is disabled we still need the reflection buffer in a defined
    // state for the composite pass. The cheapest way is a single render-pass
    // begin that (with DONT_CARE load) transitions UNDEFINED -> SHADER_READ_ONLY
    // via finalLayout — but we also need actual zeroed contents. Use a
    // vkCmdClearColorImage instead (image is in UNDEFINED -> TRANSFER_DST ->
    // SHADER_READ_ONLY). Since the image is re-defined each frame the
    // UNDEFINED initial layout is fine.
    if (!m_ssrEnabled) {
        // Already primed to black SHADER_READ_ONLY — skip the redundant
        // per-frame clear. Re-prime only if the enabled path dirtied it.
        if (m_ssrFallbackPrimed) return;
        VkImageMemoryBarrier toClear{};
        toClear.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toClear.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toClear.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.srcAccessMask       = 0;
        toClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toClear.image               = m_ssrImage;
        toClear.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toClear);

        VkClearColorValue black{};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, m_ssrImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &black, 1, &range);

        VkImageMemoryBarrier toRead = toClear;
        toRead.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toRead);
        m_ssrFallbackPrimed = true;
        return;
    }

    // Enabled path dirties the SSR image; force a re-prime if SSR is later
    // disabled so the composite doesn't sample stale reflections.
    m_ssrFallbackPrimed = false;

    UpdateSSRUBO();

    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = m_ssrRenderPass;
    rp.framebuffer       = m_ssrFramebuffer;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = scExtent;
    rp.clearValueCount   = 0;  // DONT_CARE load — we write every pixel

    vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    FillFullscreenViewportScissor(command, scExtent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssrPipeline);

    VkDescriptorSet sets[2] = { m_ssrInputSets[currentFrame], m_ssrUboSets[currentFrame] };
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_ssrPipelineLayout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
}

}  // namespace RenderEngine
}  // namespace Sleak
