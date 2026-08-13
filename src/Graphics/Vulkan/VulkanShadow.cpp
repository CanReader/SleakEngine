#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "../../include/private/Graphics/Common/RenderCommandQueue.hpp"

#include <Runtime/MeshData.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include "Core/Logger.hpp"

namespace Sleak {
    namespace RenderEngine {

/// Creates the shadow depth image, sampler, render pass, and framebuffer.
bool VulkanRenderer::CreateShadowResources() {
    // 1. Create shadow depth image (2048x2048, D32_SFLOAT)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {m_shadowMapResolution, m_shadowMapResolution, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &m_shadowImage) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow map image!");
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, m_shadowImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &m_shadowImageMemory) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate shadow map memory!");
        return false;
    }

    vkBindImageMemory(device, m_shadowImage, m_shadowImageMemory, 0);

    // 2. Create image view (DEPTH aspect)
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_shadowImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_shadowImageView) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow map image view!");
        return false;
    }

    // 3. Create comparison sampler with bilinear filtering for smooth PCF
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    samplerInfo.anisotropyEnable = VK_FALSE;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_shadowSampler) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow map sampler!");
        return false;
    }

    // 3b. Create non-comparison sampler for PCSS blocker search.
    // The blocker pass needs raw depth values to average, so compareEnable is off
    // and we switch to nearest filtering to sample individual texels cleanly.
    VkSamplerCreateInfo rawSamplerInfo = samplerInfo;
    rawSamplerInfo.compareEnable = VK_FALSE;
    rawSamplerInfo.magFilter = VK_FILTER_NEAREST;
    rawSamplerInfo.minFilter = VK_FILTER_NEAREST;

    if (vkCreateSampler(device, &rawSamplerInfo, nullptr, &m_shadowRawSampler) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow map raw sampler!");
        return false;
    }

    // 4. Create depth-only render pass
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    // Dependencies for layout transitions
    std::array<VkSubpassDependency, 2> dependencies{};

    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_shadowRenderPass) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow render pass!");
        return false;
    }

    // 5. Create framebuffer
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_shadowRenderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_shadowImageView;
    fbInfo.width = m_shadowMapResolution;
    fbInfo.height = m_shadowMapResolution;
    fbInfo.layers = 1;

    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_shadowFramebuffer) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow framebuffer!");
        return false;
    }

    // 6. Transition shadow image to DEPTH_STENCIL_READ_ONLY_OPTIMAL so the
    //    descriptor is valid even before the first shadow pass runs.
    //    Depth images must use this layout (not SHADER_READ_ONLY_OPTIMAL)
    //    for sampler access; the wrong layout causes VK_ERROR_DEVICE_LOST.
    {
        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandPool = commands;
        cmdAllocInfo.commandBufferCount = 1;

        VkCommandBuffer cmdBuf;
        vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuf);

        VkCommandBufferBeginInfo cmdBeginInfo{};
        cmdBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cmdBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmdBuf, &cmdBeginInfo);

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_shadowImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmdBuf,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmdBuf);

        VkSubmitInfo layoutSubmit{};
        layoutSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        layoutSubmit.commandBufferCount = 1;
        layoutSubmit.pCommandBuffers = &cmdBuf;

        VkFenceCreateInfo layoutFenceInfo{};
        layoutFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence layoutFence;
        vkCreateFence(device, &layoutFenceInfo, nullptr, &layoutFence);
        vkQueueSubmit(graphicsQueue, 1, &layoutSubmit, layoutFence);
        vkWaitForFences(device, 1, &layoutFence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device, layoutFence, nullptr);
        vkFreeCommandBuffers(device, commands, 1, &cmdBuf);
    }

    // 7. Create shadow pipeline
    if (!CreateShadowPipeline()) {
        SLEAK_ERROR("Failed to create shadow pipeline!");
        return false;
    }

    // Write shadow sampler to set 3 descriptors (UBO resources already created).
    // Binding 0 uses the compare sampler for hardware PCF; binding 1 uses the
    // raw sampler so the PCSS blocker search can read un-compared depth values.
    if (m_lightUBOCreated && m_shadowImageView && m_shadowSampler && m_shadowRawSampler) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            // Depth images must use DEPTH_STENCIL_READ_ONLY_OPTIMAL for sampler access.
            VkDescriptorImageInfo compareInfo{};
            compareInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            compareInfo.imageView = m_shadowImageView;
            compareInfo.sampler = m_shadowSampler;

            VkDescriptorImageInfo rawInfo{};
            rawInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            rawInfo.imageView = m_shadowImageView;
            rawInfo.sampler = m_shadowRawSampler;

            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_shadowSamplerDescriptorSets[i];
            writes[0].dstBinding = 0;
            writes[0].dstArrayElement = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &compareInfo;

            writes[1] = writes[0];
            writes[1].dstBinding = 1;
            writes[1].pImageInfo = &rawInfo;

            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }

    m_shadowResourcesCreated = true;
    SLEAK_INFO("VulkanRenderer: Shadow mapping resources created ({}x{} shadow map)",
               m_shadowMapResolution, m_shadowMapResolution);
    return true;
}

/// Compiles the shadow depth shader and creates the shadow pass pipeline.
bool VulkanRenderer::CreateShadowPipeline() {
    m_shadowShader = new VulkanShader(device);
    if (!m_shadowShader->compileVertexOnly("assets/shaders/shadow_depth.vert.spv")) {
        SLEAK_ERROR("VulkanRenderer: Failed to compile shadow depth shader");
        delete m_shadowShader;
        m_shadowShader = nullptr;
        return false;
    }

    // Vertex-only pipeline (no fragment shader)
    VkPipelineShaderStageCreateInfo shaderStage = m_shadowShader->GetVertexInfo();

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Same vertex layout as main pipeline
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 7> attributeDescs{};
    attributeDescs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)};
    attributeDescs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)};
    attributeDescs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tx)};
    attributeDescs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)};
    attributeDescs[4] = {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)};
    attributeDescs[5] = {5, 0, VK_FORMAT_R32G32B32A32_SINT, offsetof(Vertex, boneIDs)};
    attributeDescs[6] = {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, boneWeights)};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT; // same winding as main pass
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 1.25f;
    rasterizer.depthBiasSlopeFactor = 1.75f;
    rasterizer.depthBiasClamp = 0.0f;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.sampleShadingEnable = VK_FALSE;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // No color blend (depth-only, no color attachment)
    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable = VK_FALSE;
    colorBlendInfo.attachmentCount = 0;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 1; // Vertex-only
    pipelineInfo.pStages = &shaderStage;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &msaa;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlendInfo;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLay;
    pipelineInfo.renderPass = m_shadowRenderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_shadowPipeline);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to create shadow pipeline!");
        return false;
    }

    SLEAK_INFO("VulkanRenderer: Shadow pipeline created successfully");
    return true;
}

/// Creates the per-frame light and shadow UBO buffers and descriptor sets.
bool VulkanRenderer::CreateShadowLightUBOResources() {
    static constexpr VkDeviceSize uboSize = sizeof(ShadowLightUBO);

    // Create per-frame UBO buffers
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = uboSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &bufferInfo, nullptr, &m_lightUBOBuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("Failed to create light UBO buffer!");
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, m_lightUBOBuffers[i], &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &m_lightUBOMemory[i]) != VK_SUCCESS) {
            SLEAK_ERROR("Failed to allocate light UBO memory!");
            return false;
        }

        vkBindBufferMemory(device, m_lightUBOBuffers[i], m_lightUBOMemory[i], 0);
        vkMapMemory(device, m_lightUBOMemory[i], 0, uboSize, 0, &m_lightUBOMapped[i]);
        memset(m_lightUBOMapped[i], 0, uboSize);
    }

    // Create descriptor pool for light UBO + shadow samplers.
    // Two shadow samplers per frame now (compare + raw for PCSS blocker search).
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT * 2; // UBO sets + sampler sets

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_lightUBODescriptorPool) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create light UBO descriptor pool!");
        return false;
    }

    // Allocate light UBO descriptor sets (set 2)
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> uboLayouts;
    uboLayouts.fill(m_lightUBODescriptorSetLayout);

    VkDescriptorSetAllocateInfo uboAllocInfo{};
    uboAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    uboAllocInfo.descriptorPool = m_lightUBODescriptorPool;
    uboAllocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    uboAllocInfo.pSetLayouts = uboLayouts.data();

    if (vkAllocateDescriptorSets(device, &uboAllocInfo, m_lightUBODescriptorSets.data()) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate light UBO descriptor sets!");
        return false;
    }

    // Write UBO descriptors
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_lightUBOBuffers[i];
        bufInfo.offset = 0;
        bufInfo.range = uboSize;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_lightUBODescriptorSets[i];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufInfo;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    // Allocate shadow sampler descriptor sets (set 3)
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> samplerLayouts;
    samplerLayouts.fill(m_shadowSamplerDescriptorSetLayout);

    VkDescriptorSetAllocateInfo samplerAllocInfo{};
    samplerAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    samplerAllocInfo.descriptorPool = m_lightUBODescriptorPool;
    samplerAllocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    samplerAllocInfo.pSetLayouts = samplerLayouts.data();

    if (vkAllocateDescriptorSets(device, &samplerAllocInfo, m_shadowSamplerDescriptorSets.data()) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate shadow sampler descriptor sets!");
        return false;
    }

    // Write default shadow sampler descriptors (using default texture as placeholder)
    // These will be overwritten with actual shadow map when shadow resources are created.
    // Both binding=0 (compare) and binding=1 (raw) must be populated or the layout
    // is incomplete and first-frame sampling reads undefined memory.
    if (m_defaultTexture) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = m_defaultTexture->GetImageView();
            imageInfo.sampler = m_defaultTexture->GetSampler();

            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_shadowSamplerDescriptorSets[i];
            writes[0].dstBinding = 0;
            writes[0].dstArrayElement = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &imageInfo;

            writes[1] = writes[0];
            writes[1].dstBinding = 1;

            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }

    m_lightUBOCreated = true;
    SLEAK_INFO("VulkanRenderer: Light UBO and shadow sampler resources created");
    return true;
}

/// Destroys the shadow map image, pipeline, render pass, and light UBO resources.
void VulkanRenderer::CleanupShadowResources() {
    if (m_shadowPipeline) {
        vkDestroyPipeline(device, m_shadowPipeline, nullptr);
        m_shadowPipeline = VK_NULL_HANDLE;
    }
    delete m_shadowShader;
    m_shadowShader = nullptr;

    if (m_shadowFramebuffer) {
        vkDestroyFramebuffer(device, m_shadowFramebuffer, nullptr);
        m_shadowFramebuffer = VK_NULL_HANDLE;
    }
    if (m_shadowRenderPass) {
        vkDestroyRenderPass(device, m_shadowRenderPass, nullptr);
        m_shadowRenderPass = VK_NULL_HANDLE;
    }
    if (m_shadowSampler) {
        vkDestroySampler(device, m_shadowSampler, nullptr);
        m_shadowSampler = VK_NULL_HANDLE;
    }
    if (m_shadowRawSampler) {
        vkDestroySampler(device, m_shadowRawSampler, nullptr);
        m_shadowRawSampler = VK_NULL_HANDLE;
    }
    if (m_shadowImageView) {
        vkDestroyImageView(device, m_shadowImageView, nullptr);
        m_shadowImageView = VK_NULL_HANDLE;
    }
    if (m_shadowImage) {
        vkDestroyImage(device, m_shadowImage, nullptr);
        m_shadowImage = VK_NULL_HANDLE;
    }
    if (m_shadowImageMemory) {
        vkFreeMemory(device, m_shadowImageMemory, nullptr);
        m_shadowImageMemory = VK_NULL_HANDLE;
    }

    // Cleanup light UBO resources
    if (m_lightUBOCreated) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (m_lightUBOMapped[i]) {
                vkUnmapMemory(device, m_lightUBOMemory[i]);
                m_lightUBOMapped[i] = nullptr;
            }
            if (m_lightUBOBuffers[i]) {
                vkDestroyBuffer(device, m_lightUBOBuffers[i], nullptr);
                m_lightUBOBuffers[i] = VK_NULL_HANDLE;
            }
            if (m_lightUBOMemory[i]) {
                vkFreeMemory(device, m_lightUBOMemory[i], nullptr);
                m_lightUBOMemory[i] = VK_NULL_HANDLE;
            }
        }
        m_lightUBOCreated = false;
    }

    if (m_lightUBODescriptorPool) {
        vkDestroyDescriptorPool(device, m_lightUBODescriptorPool, nullptr);
        m_lightUBODescriptorPool = VK_NULL_HANDLE;
    }

    m_shadowResourcesCreated = false;
}

/// Marks the shadow pass active and invalidates the push constant cache.
void VulkanRenderer::BeginShadowPass() {
    m_shadowPassActive = true;
    m_shadowPCCacheValid = false;
}

/// Marks the shadow pass inactive.
void VulkanRenderer::EndShadowPass() {
    m_shadowPassActive = false;
}

/// Copies light and shadow data into the current frame's mapped UBO.
void VulkanRenderer::UpdateShadowLightUBO(const void* data, uint32_t size) {
    if (!m_lightUBOCreated || !data) return;
    uint32_t copySize = std::min(size, static_cast<uint32_t>(sizeof(ShadowLightUBO)));
    memcpy(m_lightUBOMapped[currentFrame], data, copySize);
}

/// Stages the light view-projection matrix for commit at the next BeginRender.
void VulkanRenderer::SetLightVP(const float* lightVP) {
    // Stage only — commit happens at the next BeginRender. This keeps
    // m_lightVP frozen for the duration of a frame so the shadow pass and
    // the main pass agree on the transform (fixes per-frame shadow jitter
    // caused by LightManager::UpdateAndBind mutating m_lightVP mid-frame,
    // between the shadow pass and the main pass).
    if (lightVP) {
        memcpy(m_pendingLightVP, lightVP, sizeof(m_pendingLightVP));
        m_hasPendingLightVP = true;
    }
}

}  // namespace RenderEngine
}  // namespace Sleak
