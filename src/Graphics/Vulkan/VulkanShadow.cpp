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
    if (!CreateShadowMapImage()) return false;

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

    if (!CreateShadowFramebuffers()) return false;

    // 7. Create shadow pipeline
    if (!CreateShadowPipeline()) {
        SLEAK_ERROR("Failed to create shadow pipeline!");
        return false;
    }

    WriteShadowSamplerDescriptors();

    m_shadowResourcesCreated = true;
    SLEAK_INFO(
        "VulkanRenderer: Shadow mapping resources created ({}x{}, {} cascades)",
        m_shadowMapResolution, m_shadowMapResolution, m_shadowCascadeCount);
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

    std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR,
                                                 VK_DYNAMIC_STATE_DEPTH_BIAS};

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
    rasterizer.depthBiasEnable = VK_TRUE;  // factors set per pass

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

    // Create descriptor pool for light UBO + shadow samplers: compare + raw
    // for the last cascade (2D) and for all cascades (array).
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * 4;

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

    // Placeholder descriptors (the default texture) until the shadow map
    // exists. Every binding must be populated or the layout is incomplete and
    // first-frame sampling reads undefined memory.
    if (m_defaultTexture) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = m_defaultTexture->GetImageView();
            imageInfo.sampler = m_defaultTexture->GetSampler();

            std::array<VkWriteDescriptorSet, 4> writes{};
            for (uint32_t b = 0; b < writes.size(); ++b) {
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet = m_shadowSamplerDescriptorSets[i];
                writes[b].dstBinding = b;
                writes[b].descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[b].descriptorCount = 1;
                writes[b].pImageInfo = &imageInfo;
            }

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
    DestroyShadowMapImage();

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

/// Stages the per-cascade light view-projections; BeginRender commits them.
void VulkanRenderer::SetShadowCascades(const float* viewProj, uint32_t count) {
    count = std::min(count, MAX_SHADOW_CASCADES);
    if (count > 0 && viewProj)
        memcpy(m_pendingCascadeVP, viewProj, sizeof(float) * 16 * count);
    m_pendingCascadeCount = count;
}

/// Creates the layered shadow image and its array and per-layer views.
bool VulkanRenderer::CreateShadowMapImage() {
    const uint32_t layers = m_shadowCascadeCount;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {m_shadowMapResolution, m_shadowMapResolution, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layers;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &m_shadowImage) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow map image!");
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, m_shadowImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &m_shadowImageMemory) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate shadow map memory!");
        return false;
    }
    vkBindImageMemory(device, m_shadowImage, m_shadowImageMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_shadowImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = layers;

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_shadowArrayView) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow map array view!");
        return false;
    }

    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.subresourceRange.layerCount = 1;
    for (uint32_t i = 0; i < layers; ++i) {
        viewInfo.subresourceRange.baseArrayLayer = i;
        if (vkCreateImageView(device, &viewInfo, nullptr,
                              &m_shadowLayerViews[i]) != VK_SUCCESS) {
            SLEAK_ERROR("Failed to create shadow map layer view!");
            return false;
        }
    }
    // Shaders that only know one shadow map sample the last cascade.
    m_shadowImageView = m_shadowLayerViews[layers - 1];

    // Transition every layer to DEPTH_STENCIL_READ_ONLY_OPTIMAL so the
    // descriptors are valid before the first shadow pass runs. Depth images
    // must use this layout (not SHADER_READ_ONLY_OPTIMAL) for sampler access.
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
        barrier.subresourceRange.layerCount = layers;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);

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

    return true;
}

/// Creates one framebuffer per cascade layer for the shadow render pass.
bool VulkanRenderer::CreateShadowFramebuffers() {
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_shadowRenderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.width = m_shadowMapResolution;
    fbInfo.height = m_shadowMapResolution;
    fbInfo.layers = 1;

    for (uint32_t i = 0; i < m_shadowCascadeCount; ++i) {
        fbInfo.pAttachments = &m_shadowLayerViews[i];
        if (vkCreateFramebuffer(device, &fbInfo, nullptr,
                                &m_shadowFramebuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("Failed to create shadow framebuffer!");
            return false;
        }
    }
    return true;
}

/// Destroys the shadow image, its views, and the per-cascade framebuffers.
void VulkanRenderer::DestroyShadowMapImage() {
    for (auto& fb : m_shadowFramebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
        fb = VK_NULL_HANDLE;
    }
    for (auto& view : m_shadowLayerViews) {
        if (view) vkDestroyImageView(device, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    m_shadowImageView = VK_NULL_HANDLE;
    if (m_shadowArrayView) {
        vkDestroyImageView(device, m_shadowArrayView, nullptr);
        m_shadowArrayView = VK_NULL_HANDLE;
    }
    if (m_shadowImage) {
        vkDestroyImage(device, m_shadowImage, nullptr);
        m_shadowImage = VK_NULL_HANDLE;
    }
    if (m_shadowImageMemory) {
        vkFreeMemory(device, m_shadowImageMemory, nullptr);
        m_shadowImageMemory = VK_NULL_HANDLE;
    }
}

/// Points set 3 at the shadow views: bindings 0/1 see the last cascade as a
/// plain 2D map, bindings 2/3 see every cascade as an array.
void VulkanRenderer::WriteShadowSamplerDescriptors() {
    if (!m_lightUBOCreated || !m_shadowImageView || !m_shadowArrayView ||
        !m_shadowSampler || !m_shadowRawSampler)
        return;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        std::array<VkDescriptorImageInfo, 4> infos{};
        for (auto& info : infos)
            info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[0].imageView = m_shadowImageView;
        infos[0].sampler = m_shadowSampler;
        infos[1].imageView = m_shadowImageView;
        infos[1].sampler = m_shadowRawSampler;
        infos[2].imageView = m_shadowArrayView;
        infos[2].sampler = m_shadowSampler;
        infos[3].imageView = m_shadowArrayView;
        infos[3].sampler = m_shadowRawSampler;

        std::array<VkWriteDescriptorSet, 4> writes{};
        for (uint32_t b = 0; b < writes.size(); ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = m_shadowSamplerDescriptorSets[i];
            writes[b].dstBinding = b;
            writes[b].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].descriptorCount = 1;
            writes[b].pImageInfo = &infos[b];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

/// Renders the cached shadow casters once per cascade, each into its own
/// layer with that cascade's light VP behind the shadow push constants.
void VulkanRenderer::RecordShadowPass() {
    auto* queue = RenderCommandQueue::GetInstance();
    if (!m_shadowResourcesCreated || !m_shadowPassEnabled || !queue ||
        !queue->HasCachedShadowDraws())
        return;

    const uint32_t cascades = std::min(m_cascadeCount, m_shadowCascadeCount);

    VkClearValue shadowClear{};
    shadowClear.depthStencil = {1.0f, 0};

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_shadowMapResolution);
    viewport.height = static_cast<float>(m_shadowMapResolution);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {m_shadowMapResolution, m_shadowMapResolution};

    for (uint32_t c = 0; c < cascades; ++c) {
        VkRenderPassBeginInfo passInfo{};
        passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        passInfo.renderPass = m_shadowRenderPass;
        passInfo.framebuffer = m_shadowFramebuffers[c];
        passInfo.renderArea.extent = scissor.extent;
        passInfo.clearValueCount = 1;
        passInfo.pClearValues = &shadowClear;

        vkCmdBeginRenderPass(command, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_shadowPipeline);
        m_activeCustomFormat = 0;
        m_customFormatUnbound = false;

        // shadow_depth.vert statically declares the set-1 BoneUBO, so it must
        // be bound even for static casters (VUID-vkCmdDrawIndexed-None-08600).
        if (m_boneUBOCreated) {
            vkCmdBindDescriptorSets(
                command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 1, 1,
                &boneDescriptorSets[currentFrame], 0, nullptr);
        }

        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdSetDepthBias(command, 1.25f * m_shadowDepthBiasScale, 0.0f,
                          1.75f * m_shadowDepthBiasScale);

        memcpy(m_lightVP, m_cascadeVP[c], sizeof(m_lightVP));
        m_shadowPassActive = true;
        m_shadowPCCacheValid = false;
        queue->ExecuteShadowPass(this);
        m_shadowPassActive = false;

        vkCmdEndRenderPass(command);
    }
}

}  // namespace RenderEngine
}  // namespace Sleak
