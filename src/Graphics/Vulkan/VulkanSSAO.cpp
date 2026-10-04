#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"

#include <random>
#include <cmath>
#include <array>
#include <cstring>
#include <vector>
#include "Core/Logger.hpp"

namespace Sleak {
    namespace RenderEngine {

// ==================================================================
// ==================== SSAO (HBAO-quality) =========================
// ==================================================================
// Half-resolution hemisphere AO with a 32-sample cosine-weighted kernel,
// 4x4 random rotation tile, and depth-aware bilateral blur.

/// Declares the raw and blurred SSAO targets and passes, and creates the
/// samplers.
bool VulkanRenderer::CreateSSAOTargets() {
    // Full-resolution SSAO, half-res caused a visible seam at the center
    // texel boundary. TRANSFER_DST enables vkCmdClearColorImage when SSAO
    // is disabled (the lighting pass always samples the blur image
    // regardless).
    VulkanPostGraph::TargetDesc desc;
    desc.format = m_ssaoFormat;
    desc.extraUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    desc.name = "ssao_raw";
    m_ssaoRawTarget = m_postGraph.AddTarget(desc);
    desc.name = "ssao_blur";
    m_ssaoBlurTarget = m_postGraph.AddTarget(desc);

    VulkanPostGraph::PassDesc pass;
    pass.name = "ssao";
    pass.phase = kPostPhaseSSAO;
    pass.output = {m_ssaoRawTarget, 0};
    pass.record = [this](VkCommandBuffer cmd, VkExtent2D) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_ssaoPipeline);
        VkDescriptorSet sets[2] = {m_ssaoInputSets[currentFrame],
                                   m_ssaoUboSets[currentFrame]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_ssaoPipelineLayout, 0, 2, sets, 0,
                                nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    };
    m_ssaoPass = m_postGraph.AddPass(pass);

    pass.name = "ssao_blur";
    pass.output = {m_ssaoBlurTarget, 0};
    pass.reads = {{m_ssaoRawTarget, 0}};
    pass.record = [this](VkCommandBuffer cmd, VkExtent2D) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_ssaoBlurPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_ssaoBlurPipelineLayout, 0, 1,
                                &m_ssaoBlurSets[currentFrame], 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    };
    m_ssaoBlurPass = m_postGraph.AddPass(pass);

    // Linear clamp sampler used by all SSAO consumers (the lighting pass
    // samples at full res, linear reconstructs the half-res buffer
    // smoothly).
    VkSamplerCreateInfo ls{};
    ls.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ls.magFilter = VK_FILTER_LINEAR;
    ls.minFilter = VK_FILTER_LINEAR;
    ls.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ls.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ls.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ls.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ls.minLod = 0.0f;
    ls.maxLod = 0.0f;
    if (vkCreateSampler(device, &ls, nullptr, &m_ssaoSampler) != VK_SUCCESS)
        return false;

    // Point sampler for depth input (we want nearest to avoid bilinear
    // bleed across silhouettes when reading the depth buffer).
    VkSamplerCreateInfo ps{};
    ps.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ps.magFilter = VK_FILTER_NEAREST;
    ps.minFilter = VK_FILTER_NEAREST;
    ps.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ps.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.minLod = 0.0f;
    ps.maxLod = 0.0f;
    if (vkCreateSampler(device, &ps, nullptr, &m_ssaoPointSampler) !=
        VK_SUCCESS)
        return false;

    return true;
}

/// Creates the SSAO input/UBO/blur descriptor layouts, pool, sets, and UBO buffers.
bool VulkanRenderer::CreateSSAODescriptorResources() {
    // Set 0 for SSAO: bindings 0..2 (gNormalRough, gDepth, noise).
    // World position is reconstructed from gDepth + InvViewProj (set 1 UBO).
    {
        std::array<VkDescriptorSetLayoutBinding, 3> binds{};
        for (uint32_t i = 0; i < 3; ++i) {
            binds[i].binding         = i;
            binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(binds.size());
        info.pBindings    = binds.data();
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_ssaoInputDSL) != VK_SUCCESS) return false;
    }
    // Set 1 for SSAO: UBO (kernel, matrices, params).
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
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_ssaoUboDSL) != VK_SUCCESS) return false;
    }
    // Set 0 for SSAO blur: bindings 0 (raw SSAO), 1 (depth).
    {
        std::array<VkDescriptorSetLayoutBinding, 2> binds{};
        for (uint32_t i = 0; i < 2; ++i) {
            binds[i].binding         = i;
            binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(binds.size());
        info.pBindings    = binds.data();
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_ssaoBlurDSL) != VK_SUCCESS) return false;
    }

    // Pool: (4 samplers + 2 samplers) * 2 sets per frame + 1 UBO per frame.
    std::array<VkDescriptorPoolSize, 2> sizes{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = (4 + 2) * MAX_FRAMES_IN_FLIGHT;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pool.pPoolSizes    = sizes.data();
    pool.maxSets       = 3 * MAX_FRAMES_IN_FLIGHT; // input + ubo + blur
    if (vkCreateDescriptorPool(device, &pool, nullptr, &m_ssaoDescriptorPool) != VK_SUCCESS) return false;

    // Allocate input (set 0) + UBO (set 1) + blur (set 0) for each frame slot.
    auto allocSets = [&](VkDescriptorSetLayout dsl, std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>& out) -> bool {
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
        layouts.fill(dsl);
        VkDescriptorSetAllocateInfo a{};
        a.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        a.descriptorPool     = m_ssaoDescriptorPool;
        a.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        a.pSetLayouts        = layouts.data();
        return vkAllocateDescriptorSets(device, &a, out.data()) == VK_SUCCESS;
    };
    if (!allocSets(m_ssaoInputDSL, m_ssaoInputSets)) return false;
    if (!allocSets(m_ssaoUboDSL,   m_ssaoUboSets))   return false;
    if (!allocSets(m_ssaoBlurDSL,  m_ssaoBlurSets))  return false;

    // Create SSAO UBO buffers (per frame).
    static constexpr VkDeviceSize uboSize = sizeof(SSAOParams);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = uboSize;
        bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &m_ssaoUboBuffers[i]) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, m_ssaoUboBuffers[i], &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device, &alloc, nullptr, &m_ssaoUboMemory[i]) != VK_SUCCESS) return false;
        vkBindBufferMemory(device, m_ssaoUboBuffers[i], m_ssaoUboMemory[i], 0);
        if (vkMapMemory(device, m_ssaoUboMemory[i], 0, uboSize, 0, &m_ssaoUboMapped[i]) != VK_SUCCESS) return false;

        // Bind UBO to set 1 descriptor.
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_ssaoUboBuffers[i];
        bufInfo.offset = 0;
        bufInfo.range  = uboSize;

        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_ssaoUboSets[i];
        w.dstBinding      = 0;
        w.dstArrayElement = 0;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo     = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }

    return true;
}

/// Generates and uploads the 4x4 tangent-plane rotation noise texture.
bool VulkanRenderer::CreateSSAONoiseTexture() {
    // 4x4 RGBA8 noise — random tangent-plane rotation vectors with Z=0
    // (they live in the tangent plane of the surface).
    const uint32_t noiseCount = SSAO_NOISE_SIZE * SSAO_NOISE_SIZE;
    std::array<uint8_t, noiseCount * 4> pixels{};

    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (uint32_t i = 0; i < noiseCount; ++i) {
        float x = dist(rng) * 2.0f - 1.0f;
        float y = dist(rng) * 2.0f - 1.0f;
        // Remap [-1,1] → [0,255] via (v * 0.5 + 0.5) * 255.
        pixels[i * 4 + 0] = static_cast<uint8_t>((x * 0.5f + 0.5f) * 255.0f);
        pixels[i * 4 + 1] = static_cast<uint8_t>((y * 0.5f + 0.5f) * 255.0f);
        pixels[i * 4 + 2] = 128;           // Z = 0
        pixels[i * 4 + 3] = 255;
    }

    // Create image.
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent.width  = SSAO_NOISE_SIZE;
    info.extent.height = SSAO_NOISE_SIZE;
    info.extent.depth  = 1;
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    if (!VulkanPostGraph::CreateImage(VulkanBuffer::GetAllocator(), info,
                                      m_ssaoNoiseImage, m_ssaoNoiseAllocation))
        return false;

    VkImageViewCreateInfo vinfo{};
    vinfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vinfo.image    = m_ssaoNoiseImage;
    vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vinfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
    vinfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vinfo, nullptr, &m_ssaoNoiseView) != VK_SUCCESS) return false;

    VulkanBuffer::PendingStagingCleanup staging;
    if (!VulkanBuffer::CreateStagingBuffer(pixels.data(), pixels.size(),
                                           staging))
        return false;

    // Single-shot command buffer for upload.
    VulkanImmediateSubmit::Run([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b0{};
        b0.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b0.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b0.image = m_ssaoNoiseImage;
        b0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b0.srcAccessMask = 0;
        b0.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b0);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {SSAO_NOISE_SIZE, SSAO_NOISE_SIZE, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, m_ssaoNoiseImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);

        VkImageMemoryBarrier b1 = b0;
        b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b1);
    });
    VulkanBuffer::DestroyStagingBuffer(staging);

    // Noise sampler — repeat (we want the 4x4 tile to wrap across the screen).
    VkSamplerCreateInfo ns{};
    ns.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ns.magFilter    = VK_FILTER_NEAREST;
    ns.minFilter    = VK_FILTER_NEAREST;
    ns.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ns.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ns.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ns.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ns.minLod       = 0.0f;
    ns.maxLod       = 0.0f;
    if (vkCreateSampler(device, &ns, nullptr, &m_ssaoNoiseSampler) != VK_SUCCESS) return false;

    return true;
}

/// Compiles the SSAO and SSAO-blur shaders and creates their pipelines.
bool VulkanRenderer::CreateSSAOPipelines() {
    // ---- SSAO main pipeline ----
    m_ssaoShader = new VulkanShader(device);
    if (!m_ssaoShader->compile("assets/shaders/ssao.vert.spv",
                                "assets/shaders/ssao.frag.spv")) {
        SLEAK_ERROR("SSAO: failed to compile ssao shaders");
        return false;
    }

    std::array<VkDescriptorSetLayout, 2> ssaoLayouts = { m_ssaoInputDSL, m_ssaoUboDSL };
    VkPipelineLayoutCreateInfo pli{};
    pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = static_cast<uint32_t>(ssaoLayouts.size());
    pli.pSetLayouts    = ssaoLayouts.data();
    if (vkCreatePipelineLayout(device, &pli, nullptr, &m_ssaoPipelineLayout) != VK_SUCCESS) {
        SLEAK_ERROR("SSAO: failed to create ssao pipeline layout");
        return false;
    }

    // Common pipeline state for all full-screen post-process shaders.
    VkPipelineShaderStageCreateInfo ssaoStages[] = {
        m_ssaoShader->GetVertexInfo(),
        m_ssaoShader->GetFragInfo()
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
    gpi.pStages             = ssaoStages;
    gpi.pVertexInputState   = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &dss;
    gpi.pColorBlendState    = &cb;
    gpi.pDynamicState       = &ds;
    gpi.layout              = m_ssaoPipelineLayout;
    gpi.renderPass = m_postGraph.GetRenderPass(m_ssaoPass);
    gpi.subpass             = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_ssaoPipeline) != VK_SUCCESS) {
        SLEAK_ERROR("SSAO: failed to create ssao pipeline");
        return false;
    }

    // ---- SSAO blur pipeline ----
    m_ssaoBlurShader = new VulkanShader(device);
    if (!m_ssaoBlurShader->compile("assets/shaders/ssao_blur.vert.spv",
                                    "assets/shaders/ssao_blur.frag.spv")) {
        SLEAK_ERROR("SSAO: failed to compile ssao_blur shaders");
        return false;
    }

    VkPipelineLayoutCreateInfo pliBlur{};
    pliBlur.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pliBlur.setLayoutCount = 1;
    pliBlur.pSetLayouts    = &m_ssaoBlurDSL;
    if (vkCreatePipelineLayout(device, &pliBlur, nullptr, &m_ssaoBlurPipelineLayout) != VK_SUCCESS) {
        SLEAK_ERROR("SSAO: failed to create ssao blur pipeline layout");
        return false;
    }

    VkPipelineShaderStageCreateInfo blurStages[] = {
        m_ssaoBlurShader->GetVertexInfo(),
        m_ssaoBlurShader->GetFragInfo()
    };
    gpi.pStages  = blurStages;
    gpi.layout   = m_ssaoBlurPipelineLayout;
    gpi.renderPass = m_postGraph.GetRenderPass(m_ssaoBlurPass);
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_ssaoBlurPipeline) != VK_SUCCESS) {
        SLEAK_ERROR("SSAO: failed to create ssao blur pipeline");
        return false;
    }

    return true;
}

/// Creates all SSAO images, render pass, framebuffers, descriptors, and pipelines.
bool VulkanRenderer::CreateSSAOResources() {
    if (m_ssaoResourcesCreated) return true;

    if (!CreateSSAOTargets()) {
        SLEAK_ERROR("SSAO: targets failed");
        return false;
    }
    if (!CreateSSAONoiseTexture())         { SLEAK_ERROR("SSAO: noise failed");         return false; }
    if (!CreateSSAODescriptorResources())  { SLEAK_ERROR("SSAO: descriptors failed");   return false; }
    if (!CreateSSAOPipelines())            { SLEAK_ERROR("SSAO: pipelines failed");     return false; }

    m_ssaoResourcesCreated = true;
    SLEAK_INFO("SSAO resources created ({}x{})", scExtent.width,
               scExtent.height);
    return true;
}

/// Destroys the SSAO pipelines, descriptors, noise texture, and samplers.
void VulkanRenderer::CleanupSSAOResources() {
    if (!m_ssaoResourcesCreated) return;

    if (m_ssaoPipeline)              { vkDestroyPipeline(device, m_ssaoPipeline, nullptr);              m_ssaoPipeline = VK_NULL_HANDLE; }
    if (m_ssaoBlurPipeline)          { vkDestroyPipeline(device, m_ssaoBlurPipeline, nullptr);          m_ssaoBlurPipeline = VK_NULL_HANDLE; }
    if (m_ssaoPipelineLayout)        { vkDestroyPipelineLayout(device, m_ssaoPipelineLayout, nullptr);  m_ssaoPipelineLayout = VK_NULL_HANDLE; }
    if (m_ssaoBlurPipelineLayout)    { vkDestroyPipelineLayout(device, m_ssaoBlurPipelineLayout, nullptr); m_ssaoBlurPipelineLayout = VK_NULL_HANDLE; }
    delete m_ssaoShader;     m_ssaoShader     = nullptr;
    delete m_ssaoBlurShader; m_ssaoBlurShader = nullptr;

    if (m_ssaoDescriptorPool)  { vkDestroyDescriptorPool(device, m_ssaoDescriptorPool, nullptr); m_ssaoDescriptorPool = VK_NULL_HANDLE; }
    if (m_ssaoInputDSL)        { vkDestroyDescriptorSetLayout(device, m_ssaoInputDSL, nullptr); m_ssaoInputDSL = VK_NULL_HANDLE; }
    if (m_ssaoUboDSL)          { vkDestroyDescriptorSetLayout(device, m_ssaoUboDSL,   nullptr); m_ssaoUboDSL   = VK_NULL_HANDLE; }
    if (m_ssaoBlurDSL)         { vkDestroyDescriptorSetLayout(device, m_ssaoBlurDSL,  nullptr); m_ssaoBlurDSL  = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_ssaoUboMapped[i])  { vkUnmapMemory(device, m_ssaoUboMemory[i]); m_ssaoUboMapped[i] = nullptr; }
        if (m_ssaoUboBuffers[i]) { vkDestroyBuffer(device, m_ssaoUboBuffers[i], nullptr); m_ssaoUboBuffers[i] = VK_NULL_HANDLE; }
        if (m_ssaoUboMemory[i])  { vkFreeMemory(device, m_ssaoUboMemory[i], nullptr);     m_ssaoUboMemory[i]  = VK_NULL_HANDLE; }
    }

    if (m_ssaoNoiseView)    { vkDestroyImageView(device, m_ssaoNoiseView, nullptr); m_ssaoNoiseView = VK_NULL_HANDLE; }
    if (m_ssaoNoiseImage) {
        vmaDestroyImage(VulkanBuffer::GetAllocator(), m_ssaoNoiseImage,
                        m_ssaoNoiseAllocation);
        m_ssaoNoiseImage = VK_NULL_HANDLE;
        m_ssaoNoiseAllocation = VK_NULL_HANDLE;
    }
    if (m_ssaoNoiseSampler) { vkDestroySampler(device, m_ssaoNoiseSampler, nullptr); m_ssaoNoiseSampler = VK_NULL_HANDLE; }

    if (m_ssaoSampler)      { vkDestroySampler(device, m_ssaoSampler, nullptr);      m_ssaoSampler = VK_NULL_HANDLE; }
    if (m_ssaoPointSampler) { vkDestroySampler(device, m_ssaoPointSampler, nullptr); m_ssaoPointSampler = VK_NULL_HANDLE; }

    m_ssaoResourcesCreated = false;
}

/// Writes the GBuffer, depth, and noise samplers into the SSAO input and blur descriptor sets.
void VulkanRenderer::UpdateSSAODescriptors() {
    if (!m_ssaoResourcesCreated) return;

    // Write per-frame descriptors. Do all frame slots now — called during
    // init before any frames are recorded, so no concurrent GPU reads.
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        // Set 0 (input samplers): gNormalRough, gDepth, noise.
        std::array<VkDescriptorImageInfo, 3> inputInfos{};
        // gNormalRough = gbuffer[1]; world position reconstructed from depth.
        inputInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        inputInfos[0].imageView   = m_gbufferViews[1];
        inputInfos[0].sampler     = m_ssaoPointSampler;

        inputInfos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        inputInfos[1].imageView   = depthImageView;
        inputInfos[1].sampler     = m_ssaoPointSampler;

        inputInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        inputInfos[2].imageView   = m_ssaoNoiseView;
        inputInfos[2].sampler     = m_ssaoNoiseSampler;

        std::array<VkWriteDescriptorSet, 3> inputWrites{};
        for (uint32_t i = 0; i < 3; ++i) {
            inputWrites[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            inputWrites[i].dstSet          = m_ssaoInputSets[f];
            inputWrites[i].dstBinding      = i;
            inputWrites[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            inputWrites[i].descriptorCount = 1;
            inputWrites[i].pImageInfo      = &inputInfos[i];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(inputWrites.size()),
                               inputWrites.data(), 0, nullptr);

        // Blur set 0: raw SSAO + depth.
        std::array<VkDescriptorImageInfo, 2> blurInfos{};
        blurInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        blurInfos[0].imageView = m_postGraph.GetView({m_ssaoRawTarget, 0});
        blurInfos[0].sampler     = m_ssaoSampler;

        blurInfos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        blurInfos[1].imageView   = depthImageView;
        blurInfos[1].sampler     = m_ssaoPointSampler;

        std::array<VkWriteDescriptorSet, 2> blurWrites{};
        for (uint32_t i = 0; i < 2; ++i) {
            blurWrites[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            blurWrites[i].dstSet          = m_ssaoBlurSets[f];
            blurWrites[i].dstBinding      = i;
            blurWrites[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            blurWrites[i].descriptorCount = 1;
            blurWrites[i].pImageInfo      = &blurInfos[i];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(blurWrites.size()),
                               blurWrites.data(), 0, nullptr);
    }
}

/// Fills the SSAO UBO with the cached camera matrices and a cosine-weighted hemisphere kernel.
void VulkanRenderer::UpdateSSAOUBO() {
    if (!m_ssaoResourcesCreated || !m_ssaoUboMapped[currentFrame]) return;

    SSAOParams p{};
    memcpy(p.View,        m_cachedView,        sizeof(p.View));
    memcpy(p.Projection,  m_cachedProjection,  sizeof(p.Projection));
    memcpy(p.InvViewProj, m_cachedInvViewProj, sizeof(p.InvViewProj));

    // Generate cosine-weighted hemisphere kernel — we do this once in a
    // session (use a fixed RNG seed). The kernel vectors are in the tangent
    // space of the surface: Z points along the normal.
    static bool s_kernelInit = false;
    static float s_kernel[SSAO_KERNEL_SIZE][4];
    if (!s_kernelInit) {
        std::mt19937 rng(20240520u);
        std::uniform_real_distribution<float> d(0.0f, 1.0f);
        for (uint32_t i = 0; i < SSAO_KERNEL_SIZE; ++i) {
            float x = d(rng) * 2.0f - 1.0f;
            float y = d(rng) * 2.0f - 1.0f;
            float z = d(rng);             // positive Z — hemisphere
            float len = std::sqrt(x*x + y*y + z*z);
            if (len < 1e-6f) { x = 0.0f; y = 0.0f; z = 1.0f; len = 1.0f; }
            x /= len; y /= len; z /= len;
            float scale = float(i) / float(SSAO_KERNEL_SIZE);
            // Bias samples closer to the origin (quadratic distance falloff).
            scale = 0.1f + 0.9f * scale * scale;
            s_kernel[i][0] = x * scale;
            s_kernel[i][1] = y * scale;
            s_kernel[i][2] = z * scale;
            s_kernel[i][3] = 0.0f;
        }
        s_kernelInit = true;
    }
    memcpy(p.Kernel, s_kernel, sizeof(p.Kernel));

    p.ScreenW = static_cast<float>(scExtent.width);
    p.ScreenH = static_cast<float>(scExtent.height);
    // Noise tile scale: screen pixels / noise texture size so the 4x4 noise tiles naturally.
    p.NoiseScaleX = static_cast<float>(scExtent.width)  / float(SSAO_NOISE_SIZE);
    p.NoiseScaleY = static_cast<float>(scExtent.height) / float(SSAO_NOISE_SIZE);

    p.Radius     = 0.5f;    // 0.5m world-space hemisphere
    p.Bias       = 0.012f;
    p.Power      = 2.5f;
    p.Intensity  = 1.3f;
    p.KernelSize = 16;  // sample first 16 of the 32-vec kernel (perf; no res change → no seam)

    memcpy(m_ssaoUboMapped[currentFrame], &p, sizeof(p));
}

/// Primes the disabled-effect fallback images (ssaoBlur=white, ssr=black, bloom
/// mip0=black) once after (re)creation, leaving them SHADER_READ_ONLY. Per-frame
/// disabled paths then skip the redundant clear since the content is static.
void VulkanRenderer::InitDisabledEffectFallbacks() {
    if (!m_postGraph.IsBuilt() || m_ssrImage == VK_NULL_HANDLE) return;

    VkClearColorValue white{};
    white.float32[0] = 1.0f;
    white.float32[1] = 1.0f;
    white.float32[2] = 1.0f;
    white.float32[3] = 1.0f;
    const VkClearColorValue black{};

    const bool primed =
        VulkanImmediateSubmit::Run([&](VkCommandBuffer initCmd) {
            m_postGraph.Clear(initCmd, {m_ssaoBlurTarget, 0}, white);
            m_postGraph.Clear(initCmd, {m_bloomTarget, 0}, black);

            const VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                                0, 1};
            VkImageMemoryBarrier bar{};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.srcAccessMask = 0;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.image = m_ssrImage;
            bar.subresourceRange = sr;
            vkCmdPipelineBarrier(initCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &bar);

            vkCmdClearColorImage(initCmd, m_ssrImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
                                 1, &sr);

            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(initCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &bar);
        });
    if (!primed) return;

    m_ssaoFallbackPrimed  = true;
    m_ssrFallbackPrimed   = true;
    m_bloomFallbackPrimed = true;
}

/// Runs the raw SSAO and bilateral blur passes, or clears the blur target when SSAO is disabled.
void VulkanRenderer::RenderSSAOPasses() {
    if (!m_ssaoResourcesCreated) return;

    // SSAO disabled: the lighting pass still samples the blur target, so keep
    // it white (no occlusion). The content is static, so only re-clear after
    // the enabled path has dirtied it (runtime toggle).
    if (!m_ssaoEnabled) {
        if (m_ssaoFallbackPrimed) return;
        VkClearColorValue white{};
        white.float32[0] = 1.0f; white.float32[1] = 1.0f;
        white.float32[2] = 1.0f; white.float32[3] = 1.0f;
        m_postGraph.Clear(command, {m_ssaoBlurTarget, 0}, white);
        m_ssaoFallbackPrimed = true;
        return;
    }

    m_ssaoFallbackPrimed = false;
    UpdateSSAOUBO();
    m_postGraph.Execute(command, kPostPhaseSSAO);
}

}  // namespace RenderEngine
}  // namespace Sleak
