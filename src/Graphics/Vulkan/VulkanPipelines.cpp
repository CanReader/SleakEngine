#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"

#include <Runtime/MeshData.hpp>
#include <array>
#include <cstddef>
#include <fstream>
#include <vector>
#include "Core/Logger.hpp"

namespace Sleak {
    namespace RenderEngine {

/// Binds the skybox pipeline and its descriptor set for the current frame.
void VulkanRenderer::BeginSkyboxPass() {
    if (!bFrameStarted) return;
    // Skybox only makes sense in forward context — not inside the GBuffer geometry pass
    // where set 0 is a PBR material descriptor set incompatible with pipelineLay.
    if (m_inGeometryPass) return;
    if (skyboxPipeline == VK_NULL_HANDLE || !m_skyboxDescriptorsWritten)
        return;

    m_inVoxelPass = false;  // prevent BindVertexBuffer from overriding this pipeline
    m_activeCustomFormat = 0;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      skyboxPipeline);

    if (CurrentFrameIndex < skyboxDescriptorSets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &skyboxDescriptorSets[CurrentFrameIndex], 0, nullptr);
    }
}


/// Restores the previous pipeline and descriptor set after the skybox draw.
void VulkanRenderer::EndSkyboxPass() {
    if (!bFrameStarted) return;

    // Restore pipeline: inside geometry pass restore to the GBuffer pipeline;
    // inside the forward transparent pass restore to water/forward; otherwise main forward.
    VkPipeline restoreTo;
    if (m_inGeometryPass && m_gbufferPipeline != VK_NULL_HANDLE)
        restoreTo = m_gbufferPipeline;
    else if (m_inForwardTransparentPass && m_waterPipeline != VK_NULL_HANDLE)
        restoreTo = m_waterPipeline;
    else
        restoreTo = pipeline;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, restoreTo);

    // Rebind main texture descriptor sets.
    // In the GBuffer geometry pass set 0 belongs to m_gbufferGeomLayout and is
    // managed by BindPBRMaterial — do NOT overwrite it with the forward
    // single-sampler descriptor set or use pipelineLay here.
    if (!m_inGeometryPass && m_textureDescriptorsWritten &&
        CurrentFrameIndex < descriptorSets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &descriptorSets[CurrentFrameIndex], 0, nullptr);
    }
}

/// Creates the main forward graphics pipeline and its pipeline layout.
bool VulkanRenderer::CreateGraphicsPipeline() {
    VkResult result;

    simpleShader = new VulkanShader(device);
    bool isShader =
        simpleShader->compile("assets/shaders/default_shader");

    if (!isShader)
        SLEAK_RETURN_ERR("Cannot compile shaders!")

    VkPipelineShaderStageCreateInfo shaderStages[] = {
        simpleShader->GetVertexInfo(), simpleShader->GetFragInfo()};

    // Dynamic states
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Vertex input — matches Sleak::Vertex (64 bytes)
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 7> attributeDescs{};

    // Position: float3 at offset 0
    attributeDescs[0].binding = 0;
    attributeDescs[0].location = 0;
    attributeDescs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[0].offset = offsetof(Vertex, px);

    // Normal: float3 at offset 12
    attributeDescs[1].binding = 0;
    attributeDescs[1].location = 1;
    attributeDescs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[1].offset = offsetof(Vertex, nx);

    // Tangent: float4 at offset 24
    attributeDescs[2].binding = 0;
    attributeDescs[2].location = 2;
    attributeDescs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[2].offset = offsetof(Vertex, tx);

    // Color: float4 at offset 40
    attributeDescs[3].binding = 0;
    attributeDescs[3].location = 3;
    attributeDescs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[3].offset = offsetof(Vertex, r);

    // UV: float2 at offset 56
    attributeDescs[4].binding = 0;
    attributeDescs[4].location = 4;
    attributeDescs[4].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescs[4].offset = offsetof(Vertex, u);

    // BoneIDs: int4
    attributeDescs[5].binding = 0;
    attributeDescs[5].location = 5;
    attributeDescs[5].format = VK_FORMAT_R32G32B32A32_SINT;
    attributeDescs[5].offset = offsetof(Vertex, boneIDs);

    // BoneWeights: float4
    attributeDescs[6].binding = 0;
    attributeDescs[6].location = 6;
    attributeDescs[6].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[6].offset = offsetof(Vertex, boneWeights);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
    inputAssemblyInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    // Viewport state (dynamic)
    VkPipelineViewportStateCreateInfo viewportInfo{};
    viewportInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.viewportCount = 1;
    viewportInfo.scissorCount = 1;

    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.sampleShadingEnable = VK_FALSE;
    msaa.rasterizationSamples = m_msaaSamples;

    // Depth stencil
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Color blending (fixed: R | G | B | A, not R | G | R | A)
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable = VK_FALSE;
    colorBlendInfo.attachmentCount = 1;
    colorBlendInfo.pAttachments = &colorBlendAttachment;

    // Pipeline layout — push constants for WVP matrix + descriptor set
    // for texture sampler
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = 128;  // sizeof(mat4) * 2 = 128 bytes (WVP + World)

    // Four descriptor set layouts:
    // set 0 = texture sampler, set 1 = bone UBO,
    // set 2 = light/shadow UBO, set 3 = shadow map sampler
    std::array<VkDescriptorSetLayout, 4> setLayouts = {
        descriptorSetLayout, boneDescriptorSetLayout,
        m_lightUBODescriptorSetLayout, m_shadowSamplerDescriptorSetLayout
    };

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    result = vkCreatePipelineLayout(device, &layoutInfo, nullptr,
                                     &pipelineLay);
    if (result != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create graphics pipeline layout!");

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
    pipelineInfo.pViewportState = &viewportInfo;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &msaa;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlendInfo;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLay;
    pipelineInfo.subpass = 0;
    pipelineInfo.renderPass = (m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE)
                              ? m_forwardRenderPass : renderPass;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                        &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS)
        SLEAK_ERROR("Failed to create graphics pipeline!!");

    return true;
}


/// Creates the main forward render pass with optional MSAA color and resolve attachments.
bool VulkanRenderer::CreateRenderPass() {
    const bool msaaEnabled = (m_msaaSamples != VK_SAMPLE_COUNT_1_BIT);

    // Color attachment (multisampled when MSAA on)
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = scImageFormat;
    colorAttachment.samples = m_msaaSamples;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = msaaEnabled ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = msaaEnabled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Depth attachment (multisampled when MSAA on)
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = m_msaaSamples;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Resolve attachment (swapchain image, only when MSAA on)
    VkAttachmentDescription resolveAttachment{};
    VkAttachmentReference resolveRef{};
    if (msaaEnabled) {
        resolveAttachment.format = scImageFormat;
        resolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        resolveAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolveAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolveAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolveAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolveAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        resolveRef.attachment = 2;
        resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    // Subpass
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    subpass.pResolveAttachments = msaaEnabled ? &resolveRef : nullptr;

    // Subpass dependency
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    // Create render pass
    std::vector<VkAttachmentDescription> attachments = {
        colorAttachment, depthAttachment};
    if (msaaEnabled)
        attachments.push_back(resolveAttachment);

    VkRenderPassCreateInfo renderInfo{};
    renderInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderInfo.attachmentCount =
        static_cast<uint32_t>(attachments.size());
    renderInfo.pAttachments = attachments.data();
    renderInfo.subpassCount = 1;
    renderInfo.pSubpasses = &subpass;
    renderInfo.dependencyCount = 1;
    renderInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device, &renderInfo, nullptr, &renderPass) !=
        VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create render pass!");

    return true;
}


/// Creates one framebuffer per swapchain image for the main render pass.
bool VulkanRenderer::CreateFrameBuffer() {
    swapChainFramebuffers.resize(swapChainImageViews.size());
    const bool msaaEnabled = (m_msaaSamples != VK_SAMPLE_COUNT_1_BIT);

    for (size_t i = 0; i < swapChainImageViews.size(); i++) {
        std::vector<VkImageView> attachments;
        if (msaaEnabled) {
            // MSAA color, depth, resolve (swapchain)
            attachments = {m_msaaColorImageView, depthImageView, swapChainImageViews[i]};
        } else {
            // No MSAA: swapchain color, depth
            attachments = {swapChainImageViews[i], depthImageView};
        }

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass;
        info.attachmentCount =
            static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.layers = 1;
        info.width = scExtent.width;
        info.height = scExtent.height;

        if (vkCreateFramebuffer(device, &info, nullptr,
                                 &swapChainFramebuffers[i]) != VK_SUCCESS)
            SLEAK_RETURN_ERR("Failed to create frame buffer!");
    }
    return true;
}

/// Compiles the skybox shaders and creates the skybox descriptor set and pipeline.
bool VulkanRenderer::CreateSkyboxPipeline() {
    // 1. Compile skybox shaders
    skyboxShader = new VulkanShader(device);
    if (!skyboxShader->compile("assets/shaders/skybox")) {
        SLEAK_ERROR("VulkanRenderer: Failed to compile skybox shaders");
        delete skyboxShader;
        skyboxShader = nullptr;
        return false;
    }

    // 2. Create skybox descriptor pool and sets (same layout as main)
    uint32_t imageCount =
        static_cast<uint32_t>(swapChainImages.size());

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = imageCount;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = imageCount;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr,
                                &skyboxDescriptorPool) != VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to create skybox descriptor pool");
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(imageCount,
                                                descriptorSetLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = skyboxDescriptorPool;
    allocInfo.descriptorSetCount = imageCount;
    allocInfo.pSetLayouts = layouts.data();

    skyboxDescriptorSets.resize(imageCount);
    if (vkAllocateDescriptorSets(device, &allocInfo,
                                  skyboxDescriptorSets.data()) !=
        VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to allocate skybox descriptor sets");
        return false;
    }

    // 3. Create skybox pipeline (same as main but with skybox shaders
    //    and depth write disabled)
    VkPipelineShaderStageCreateInfo shaderStages[] = {
        skyboxShader->GetVertexInfo(), skyboxShader->GetFragInfo()};

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Same vertex layout as main pipeline
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 7> attributeDescs{};
    attributeDescs[0].binding = 0;
    attributeDescs[0].location = 0;
    attributeDescs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[0].offset = offsetof(Vertex, px);

    attributeDescs[1].binding = 0;
    attributeDescs[1].location = 1;
    attributeDescs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[1].offset = offsetof(Vertex, nx);

    attributeDescs[2].binding = 0;
    attributeDescs[2].location = 2;
    attributeDescs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[2].offset = offsetof(Vertex, tx);

    attributeDescs[3].binding = 0;
    attributeDescs[3].location = 3;
    attributeDescs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[3].offset = offsetof(Vertex, r);

    attributeDescs[4].binding = 0;
    attributeDescs[4].location = 4;
    attributeDescs[4].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescs[4].offset = offsetof(Vertex, u);

    attributeDescs[5].binding = 0;
    attributeDescs[5].location = 5;
    attributeDescs[5].format = VK_FORMAT_R32G32B32A32_SINT;
    attributeDescs[5].offset = offsetof(Vertex, boneIDs);

    attributeDescs[6].binding = 0;
    attributeDescs[6].location = 6;
    attributeDescs[6].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[6].offset = offsetof(Vertex, boneWeights);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
    inputAssemblyInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportInfo{};
    viewportInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.viewportCount = 1;
    viewportInfo.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.sampleShadingEnable = VK_FALSE;
    msaa.rasterizationSamples = m_msaaSamples;

    // Skybox: depth test enabled (LEQUAL), depth write DISABLED
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable = VK_FALSE;
    colorBlendInfo.attachmentCount = 1;
    colorBlendInfo.pAttachments = &colorBlendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
    pipelineInfo.pViewportState = &viewportInfo;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &msaa;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlendInfo;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLay;  // Reuse same pipeline layout
    pipelineInfo.subpass = 0;
    pipelineInfo.renderPass = (m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE)
                              ? m_forwardRenderPass : renderPass;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skyboxPipeline);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to create skybox pipeline!");
        return false;
    }

    SLEAK_INFO("VulkanRenderer: Skybox pipeline created successfully");
    return true;
}


/// Compiles the debug line shaders and creates the line-list pipeline.
bool VulkanRenderer::CreateDebugLinePipeline() {
    if (debugLinePipeline != VK_NULL_HANDLE) return true;

    debugLineShader = new VulkanShader(device);
    if (!debugLineShader->compile("assets/shaders/debug_line")) {
        SLEAK_ERROR("VulkanRenderer: Failed to compile debug line shaders");
        delete debugLineShader;
        debugLineShader = nullptr;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[] = {
        debugLineShader->GetVertexInfo(), debugLineShader->GetFragInfo()};

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

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
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
    inputAssemblyInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportInfo{};
    viewportInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.viewportCount = 1;
    viewportInfo.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.sampleShadingEnable = VK_FALSE;
    msaa.rasterizationSamples = m_msaaSamples;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable = VK_FALSE;
    colorBlendInfo.attachmentCount = 1;
    colorBlendInfo.pAttachments = &colorBlendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
    pipelineInfo.pViewportState = &viewportInfo;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &msaa;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlendInfo;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLay;
    pipelineInfo.subpass = 0;
    pipelineInfo.renderPass = (m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE)
                              ? m_forwardRenderPass : renderPass;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &debugLinePipeline);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to create debug line pipeline!");
        return false;
    }

    SLEAK_INFO("VulkanRenderer: Debug line pipeline created successfully");
    return true;
}


/// Compiles the optional water shaders and creates the alpha-blended water pipeline.
bool VulkanRenderer::CreateWaterPipeline() {
    if (m_waterPipeline != VK_NULL_HANDLE) return true;

    // Water shaders are optional — skip silently when not present in this project
    {
        std::ifstream vCheck("assets/shaders/water_shader.vert.spv");
        std::ifstream fCheck("assets/shaders/water_shader.frag.spv");
        if (!vCheck.good() || !fCheck.good()) return false;
    }

    m_waterShader = new VulkanShader(device);
    if (!m_waterShader->compile("assets/shaders/water_shader")) {
        SLEAK_ERROR("VulkanRenderer: Failed to compile water shaders");
        delete m_waterShader;
        m_waterShader = nullptr;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[] = {
        m_waterShader->GetVertexInfo(), m_waterShader->GetFragInfo()};

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Compact vertex input: 48-byte VoxelVertex stride, 4 attributes
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(VoxelVertex);  // 48 bytes
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributeDescs{};
    // Position: float3 at offset 0
    attributeDescs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, px))};
    // Normal: float3 at offset 12
    attributeDescs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, nx))};
    // Color: float4 at offset 24
    attributeDescs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, r))};
    // UV: float2 at offset 40
    attributeDescs[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, u))};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
    inputAssemblyInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportInfo{};
    viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.viewportCount = 1;
    viewportInfo.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    // Water is two-sided (see MainScene waterMat->SetTwoSided(true))
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.sampleShadingEnable = VK_FALSE;
    // Forward transparent pass runs on m_forwardRenderPass when deferred is
    // enabled (1 sample); use main renderPass samples otherwise.
    msaa.rasterizationSamples =
        (m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE)
            ? VK_SAMPLE_COUNT_1_BIT
            : m_msaaSamples;

    // Depth: test LESS, write ENABLED (water occludes what's behind it)
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Alpha blending: SRC_ALPHA / ONE_MINUS_SRC_ALPHA
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable = VK_FALSE;
    colorBlendInfo.attachmentCount = 1;
    colorBlendInfo.pAttachments = &colorBlendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
    pipelineInfo.pViewportState = &viewportInfo;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &msaa;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlendInfo;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLay;  // reuse main layout (same descriptor sets)
    pipelineInfo.subpass = 0;
    pipelineInfo.renderPass =
        (m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE)
            ? m_forwardRenderPass
            : renderPass;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_waterPipeline);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to create water pipeline!");
        return false;
    }

    SLEAK_INFO("VulkanRenderer: Water pipeline created successfully");
    return true;
}

// ============================================================
// Voxel pipeline — compact 48-byte VoxelVertex layout
// Used for chunk opaque meshes (flat_shader SPIR-V).
// ============================================================
/// Compiles the flat_shader SPIR-V and creates the forward and GBuffer voxel pipelines.
bool VulkanRenderer::CreateVoxelPipeline() {
    if (m_voxelPipeline != VK_NULL_HANDLE) return true;

    // Voxel shaders are optional — skip silently when not present in this project
    {
        std::ifstream vCheck("assets/shaders/flat_shader.vert.spv");
        std::ifstream fCheck("assets/shaders/flat_shader.frag.spv");
        if (!vCheck.good() || !fCheck.good()) return false;
    }

    // Compile flat_shader SPIR-V for voxel opaque rendering
    auto* voxelShader = new VulkanShader(device);
    if (!voxelShader->compile("assets/shaders/flat_shader")) {
        SLEAK_WARN("VulkanRenderer: Failed to compile flat_shader for voxel pipeline — voxels will use default pipeline");
        delete voxelShader;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[] = {
        voxelShader->GetVertexInfo(), voxelShader->GetFragInfo()};

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Compact vertex input: 48-byte stride, 4 attributes
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(VoxelVertex);  // 48 bytes
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributeDescs{};
    // Position: float3 at offset 0
    attributeDescs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, px))};
    // Normal: float3 at offset 12
    attributeDescs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, nx))};
    // Color: float4 at offset 24
    attributeDescs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, r))};
    // UV: float2 at offset 40
    attributeDescs[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, u))};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
    inputAssemblyInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportInfo{};
    viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.viewportCount = 1;
    viewportInfo.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.sampleShadingEnable = VK_FALSE;
    msaa.rasterizationSamples = m_msaaSamples;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable = VK_FALSE;
    colorBlendInfo.attachmentCount = 1;
    colorBlendInfo.pAttachments = &colorBlendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
    pipelineInfo.pViewportState = &viewportInfo;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &msaa;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlendInfo;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLay;  // reuse main layout (same descriptor sets)
    pipelineInfo.subpass = 0;
    pipelineInfo.renderPass = (m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE)
                              ? m_forwardRenderPass : renderPass;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_voxelPipeline);

    delete voxelShader;

    if (result != VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to create voxel pipeline!");
        return false;
    }

    // Also create voxel shadow pipeline
    CreateVoxelShadowPipeline();

    // Create GBuffer-compatible voxel pipeline for deferred geometry pass
    if (m_gbufferResourcesCreated && m_gbufferRenderPass != VK_NULL_HANDLE) {
        auto* gbufVoxelShader = new VulkanShader(device);
        if (gbufVoxelShader->compile("assets/shaders/gbuffer_voxel.vert.spv",
                                      "assets/shaders/gbuffer.frag.spv")) {
            VkPipelineShaderStageCreateInfo gbufStages[] = {
                gbufVoxelShader->GetVertexInfo(),
                gbufVoxelShader->GetFragInfo()};

            VkVertexInputBindingDescription voxBind{};
            voxBind.binding = 0;
            voxBind.stride = sizeof(VoxelVertex);
            voxBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            std::array<VkVertexInputAttributeDescription, 4> voxAttr{};
            // Position: float3 at offset 0
            voxAttr[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                          static_cast<uint32_t>(offsetof(VoxelVertex, px))};
            // Normal: float3 at offset 12
            voxAttr[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                          static_cast<uint32_t>(offsetof(VoxelVertex, nx))};
            // Color: float4 at offset 24
            voxAttr[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                          static_cast<uint32_t>(offsetof(VoxelVertex, r))};
            // UV: float2 at offset 40
            voxAttr[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,
                          static_cast<uint32_t>(offsetof(VoxelVertex, u))};

            VkPipelineVertexInputStateCreateInfo voxVertInfo{};
            voxVertInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            voxVertInfo.vertexBindingDescriptionCount = 1;
            voxVertInfo.pVertexBindingDescriptions = &voxBind;
            voxVertInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(voxAttr.size());
            voxVertInfo.pVertexAttributeDescriptions = voxAttr.data();

            VkPipelineInputAssemblyStateCreateInfo ia{};
            ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            ia.primitiveRestartEnable = VK_FALSE;

            VkPipelineViewportStateCreateInfo vp{};
            vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            vp.viewportCount = 1;
            vp.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.lineWidth = 1.0f;
            rs.cullMode = VK_CULL_MODE_BACK_BIT;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

            VkPipelineMultisampleStateCreateInfo ms{};
            ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_LESS;

            VkPipelineColorBlendAttachmentState opaqueBlend{};
            opaqueBlend.blendEnable = VK_FALSE;
            opaqueBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            std::array<VkPipelineColorBlendAttachmentState, GBUFFER_COUNT> gbAtts;
            gbAtts.fill(opaqueBlend);

            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = static_cast<uint32_t>(gbAtts.size());
            cb.pAttachments = gbAtts.data();

            VkGraphicsPipelineCreateInfo gbPipeInfo{};
            gbPipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gbPipeInfo.stageCount = 2;
            gbPipeInfo.pStages = gbufStages;
            gbPipeInfo.pVertexInputState = &voxVertInfo;
            gbPipeInfo.pInputAssemblyState = &ia;
            gbPipeInfo.pViewportState = &vp;
            gbPipeInfo.pRasterizationState = &rs;
            gbPipeInfo.pMultisampleState = &ms;
            gbPipeInfo.pDepthStencilState = &ds;
            gbPipeInfo.pColorBlendState = &cb;
            gbPipeInfo.pDynamicState = &dynamicState;
            // GBuffer voxel pipeline runs inside m_gbufferRenderPass and uses
            // gbuffer.frag, which reads set 0 as m_pbrMaterialDSL (6 samplers + 1 UBO).
            // Must use m_gbufferGeomLayout — NOT pipelineLay — so layout compatibility
            // is maintained for all sets when the GBuffer pipeline is active.
            gbPipeInfo.layout = m_gbufferGeomLayout;
            gbPipeInfo.renderPass = m_gbufferRenderPass;
            gbPipeInfo.subpass = 0;

            VkResult gbResult = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &gbPipeInfo, nullptr, &m_gbufferVoxelPipeline);
            if (gbResult != VK_SUCCESS) {
                SLEAK_WARN("VulkanRenderer: Failed to create GBuffer voxel pipeline");
            } else {
                SLEAK_INFO("VulkanRenderer: GBuffer voxel pipeline created");
            }
        }
        delete gbufVoxelShader;
    }

    SLEAK_INFO("VulkanRenderer: Voxel pipeline created successfully");
    return true;
}


/// Compiles the voxel shadow vertex shader and creates the voxel shadow-pass pipeline.
bool VulkanRenderer::CreateVoxelShadowPipeline() {
    if (m_voxelShadowPipeline != VK_NULL_HANDLE) return true;
    if (m_shadowRenderPass == VK_NULL_HANDLE) return false;
    if (!m_shadowShader) return false;

    // Compile voxel-specific shadow shader with compact 4-attribute layout
    auto* voxelShadowShader = new VulkanShader(device);
    if (!voxelShadowShader->compileVertexOnly("assets/shaders/shadow_depth_voxel.vert.spv")) {
        SLEAK_WARN("VulkanRenderer: Failed to compile voxel shadow shader, falling back to default");
        delete voxelShadowShader;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStage = voxelShadowShader->GetVertexInfo();

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Compact vertex input: 48-byte stride, only position needed for shadows
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(VoxelVertex);  // 48 bytes
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Compact VoxelVertex layout: 4 attributes matching the voxel shadow shader
    std::array<VkVertexInputAttributeDescription, 4> attributeDescs{};
    // loc 0: position (float3)
    attributeDescs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, px))};
    // loc 1: normal (float3)
    attributeDescs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, nx))};
    // loc 2: color (float4)
    attributeDescs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, r))};
    // loc 3: UV (float2)
    attributeDescs[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,
                         static_cast<uint32_t>(offsetof(VoxelVertex, u))};

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
    // Back-face cull: same winding as the main voxel pass, ~2x fewer
    // shadow-rasterized triangles
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
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

    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable = VK_FALSE;
    colorBlendInfo.attachmentCount = 0;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 1;
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
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_voxelShadowPipeline);
    if (result != VK_SUCCESS) {
        SLEAK_WARN("VulkanRenderer: Failed to create voxel shadow pipeline");
        return false;
    }

    delete voxelShadowShader;
    SLEAK_INFO("VulkanRenderer: Voxel shadow pipeline created successfully");
    return true;
}


/// Binds the voxel pipeline matching the currently active render pass.
void VulkanRenderer::BeginVoxelPass() {
    if (!bFrameStarted) return;
    if (m_inVoxelPass) return;
    m_inVoxelPass = true;

    if (m_voxelPipeline == VK_NULL_HANDLE) {
        if (!CreateVoxelPipeline()) return;
    }

    if (m_shadowPassActive) {
        if (m_voxelShadowPipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_voxelShadowPipeline);
        }
    } else if (m_inGeometryPass && m_gbufferVoxelPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_gbufferVoxelPipeline);
    } else if (m_inForwardTransparentPass && m_waterPipeline != VK_NULL_HANDLE) {
        // Forward transparent voxel draws are water — keep the water pipeline
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_waterPipeline);
    } else {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_voxelPipeline);
    }
}


/// Restores the previous pipeline and descriptor set after voxel draws.
void VulkanRenderer::EndVoxelPass() {
    if (!bFrameStarted) return;
    if (!m_inVoxelPass) return;
    m_inVoxelPass = false;

    if (m_shadowPassActive) {
        if (m_shadowPipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_shadowPipeline);
        }
    } else if (m_inGeometryPass && m_gbufferPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_gbufferPipeline);
    } else if (m_inForwardTransparentPass) {
        VkPipeline restoreTo = (m_waterPipeline != VK_NULL_HANDLE)
                                   ? m_waterPipeline : pipeline;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, restoreTo);
    } else {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    }

    // Re-bind descriptors after pipeline change.
    // In the GBuffer geometry pass set 0 belongs to m_gbufferGeomLayout and is
    // managed by BindPBRMaterial — do NOT overwrite it with the forward
    // single-sampler descriptor set or use pipelineLay here.
    if (!m_inGeometryPass && m_textureDescriptorsWritten &&
        CurrentFrameIndex < descriptorSets.size()) {
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLay, 0, 1,
                                &descriptorSets[CurrentFrameIndex], 0, nullptr);
    }
}


// ============================================================
// Custom vertex format pipelines — vertex input and shader stems
// come from VertexFormatRegistry instead of the hard-coded voxel layout.
// ============================================================

/// Translates a registry attribute format into its Vulkan vertex format.
static VkFormat ToVkVertexFormat(VertexAttribFormat format) {
    switch (format) {
        case VertexAttribFormat::Float1: return VK_FORMAT_R32_SFLOAT;
        case VertexAttribFormat::Float2: return VK_FORMAT_R32G32_SFLOAT;
        case VertexAttribFormat::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexAttribFormat::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VertexAttribFormat::UInt1:  return VK_FORMAT_R32_UINT;
        case VertexAttribFormat::Int1:   return VK_FORMAT_R32_SINT;
    }
    return VK_FORMAT_R32G32B32_SFLOAT;
}

/// Creates any missing pipeline variant (main, shadow, GBuffer) for a
/// registered vertex layout. Variants whose shader fails to load are marked
/// absent so draws skip them without retrying every frame.
bool VulkanRenderer::CreateCustomFormatPipelines(VertexFormatHandle format) {
    const VertexLayoutDesc* desc = VertexFormatRegistry::Get(format);
    if (!desc || desc->stride == 0 || desc->attributes.empty()) return false;

    CustomFormatPipelines& pipes = m_customFormatPipelines[format];

    // Vertex input state shared by all three variants
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = desc->stride;
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attributeDescs;
    attributeDescs.reserve(desc->attributes.size());
    for (const auto& attr : desc->attributes) {
        attributeDescs.push_back({attr.location, 0,
                                  ToVkVertexFormat(attr.format), attr.offset});
    }

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Forward/main variant — mirrors CreateVoxelPipeline
    if (pipes.main == VK_NULL_HANDLE && !pipes.mainFailed) {
        if (desc->shaderStem.empty()) {
            SLEAK_ERROR("VulkanRenderer: Vertex format {} has no main shader stem", format);
            pipes.mainFailed = true;
        } else {
            const std::string stemPath = "assets/shaders/" + desc->shaderStem;
            auto* shader = new VulkanShader(device);
            if (!shader->compile(stemPath)) {
                SLEAK_ERROR("VulkanRenderer: Failed to compile '{}' for vertex format {}",
                            stemPath, format);
                pipes.mainFailed = true;
                delete shader;
            } else {
                VkPipelineShaderStageCreateInfo shaderStages[] = {
                    shader->GetVertexInfo(), shader->GetFragInfo()};

                VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
                inputAssemblyInfo.sType =
                    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

                VkPipelineViewportStateCreateInfo viewportInfo{};
                viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                viewportInfo.viewportCount = 1;
                viewportInfo.scissorCount = 1;

                VkPipelineRasterizationStateCreateInfo rasterizer{};
                rasterizer.sType =
                    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                rasterizer.depthClampEnable = VK_FALSE;
                rasterizer.rasterizerDiscardEnable = VK_FALSE;
                rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
                rasterizer.lineWidth = 1.0f;
                rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
                rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                rasterizer.depthBiasEnable = VK_FALSE;

                VkPipelineMultisampleStateCreateInfo msaa{};
                msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                msaa.sampleShadingEnable = VK_FALSE;
                msaa.rasterizationSamples = m_msaaSamples;

                VkPipelineDepthStencilStateCreateInfo depthStencil{};
                depthStencil.sType =
                    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                depthStencil.depthTestEnable = VK_TRUE;
                depthStencil.depthWriteEnable = VK_TRUE;
                depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
                depthStencil.depthBoundsTestEnable = VK_FALSE;
                depthStencil.stencilTestEnable = VK_FALSE;

                VkPipelineColorBlendAttachmentState colorBlendAttachment{};
                colorBlendAttachment.blendEnable = VK_TRUE;
                colorBlendAttachment.colorWriteMask =
                    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                colorBlendAttachment.dstColorBlendFactor =
                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
                colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

                VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
                colorBlendInfo.sType =
                    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                colorBlendInfo.logicOpEnable = VK_FALSE;
                colorBlendInfo.attachmentCount = 1;
                colorBlendInfo.pAttachments = &colorBlendAttachment;

                VkGraphicsPipelineCreateInfo pipelineInfo{};
                pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                pipelineInfo.stageCount = 2;
                pipelineInfo.pStages = shaderStages;
                pipelineInfo.pVertexInputState = &vertexInputInfo;
                pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
                pipelineInfo.pViewportState = &viewportInfo;
                pipelineInfo.pRasterizationState = &rasterizer;
                pipelineInfo.pMultisampleState = &msaa;
                pipelineInfo.pDepthStencilState = &depthStencil;
                pipelineInfo.pColorBlendState = &colorBlendInfo;
                pipelineInfo.pDynamicState = &dynamicState;
                pipelineInfo.layout = pipelineLay;  // reuse main layout (same descriptor sets)
                pipelineInfo.subpass = 0;
                pipelineInfo.renderPass =
                    (m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE)
                        ? m_forwardRenderPass : renderPass;
                pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
                pipelineInfo.basePipelineIndex = -1;

                VkResult result = vkCreateGraphicsPipelines(
                    device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipes.main);
                delete shader;

                if (result != VK_SUCCESS) {
                    SLEAK_ERROR("VulkanRenderer: Failed to create main pipeline for vertex format {}",
                                format);
                    pipes.main = VK_NULL_HANDLE;
                    pipes.mainFailed = true;
                } else {
                    SLEAK_INFO("VulkanRenderer: Custom format {} main pipeline created", format);
                }
            }
        }
    }

    // Shadow variant — mirrors CreateVoxelShadowPipeline
    if (pipes.shadow == VK_NULL_HANDLE && !pipes.shadowFailed &&
        !desc->shadowShaderStem.empty() &&
        m_shadowRenderPass != VK_NULL_HANDLE && m_shadowShader) {
        const std::string shadowPath =
            "assets/shaders/" + desc->shadowShaderStem + ".vert.spv";
        auto* shadowShader = new VulkanShader(device);
        if (!shadowShader->compileVertexOnly(shadowPath)) {
            SLEAK_ERROR("VulkanRenderer: Failed to compile '{}' for vertex format {}",
                        shadowPath, format);
            pipes.shadowFailed = true;
            delete shadowShader;
        } else {
            VkPipelineShaderStageCreateInfo shaderStage = shadowShader->GetVertexInfo();

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
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
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

            VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
            colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlendInfo.logicOpEnable = VK_FALSE;
            colorBlendInfo.attachmentCount = 0;

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = 1;
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
                device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipes.shadow);
            delete shadowShader;

            if (result != VK_SUCCESS) {
                SLEAK_ERROR("VulkanRenderer: Failed to create shadow pipeline for vertex format {}",
                            format);
                pipes.shadow = VK_NULL_HANDLE;
                pipes.shadowFailed = true;
            } else {
                SLEAK_INFO("VulkanRenderer: Custom format {} shadow pipeline created", format);
            }
        }
    }

    // GBuffer variant — mirrors the GBuffer voxel pipeline in CreateVoxelPipeline
    if (pipes.gbuffer == VK_NULL_HANDLE && !pipes.gbufferFailed &&
        !desc->gbufferShaderStem.empty() && m_gbufferResourcesCreated &&
        m_gbufferRenderPass != VK_NULL_HANDLE) {
        const std::string gbufVertPath =
            "assets/shaders/" + desc->gbufferShaderStem + ".vert.spv";
        auto* gbufShader = new VulkanShader(device);
        if (!gbufShader->compile(gbufVertPath, "assets/shaders/gbuffer.frag.spv")) {
            SLEAK_ERROR("VulkanRenderer: Failed to compile '{}' for vertex format {}",
                        gbufVertPath, format);
            pipes.gbufferFailed = true;
            delete gbufShader;
        } else {
            VkPipelineShaderStageCreateInfo gbufStages[] = {
                gbufShader->GetVertexInfo(), gbufShader->GetFragInfo()};

            VkPipelineInputAssemblyStateCreateInfo ia{};
            ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            ia.primitiveRestartEnable = VK_FALSE;

            VkPipelineViewportStateCreateInfo vp{};
            vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            vp.viewportCount = 1;
            vp.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.lineWidth = 1.0f;
            rs.cullMode = VK_CULL_MODE_BACK_BIT;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

            VkPipelineMultisampleStateCreateInfo ms{};
            ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_LESS;

            VkPipelineColorBlendAttachmentState opaqueBlend{};
            opaqueBlend.blendEnable = VK_FALSE;
            opaqueBlend.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            std::array<VkPipelineColorBlendAttachmentState, GBUFFER_COUNT> gbAtts;
            gbAtts.fill(opaqueBlend);

            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = static_cast<uint32_t>(gbAtts.size());
            cb.pAttachments = gbAtts.data();

            VkGraphicsPipelineCreateInfo gbPipeInfo{};
            gbPipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gbPipeInfo.stageCount = 2;
            gbPipeInfo.pStages = gbufStages;
            gbPipeInfo.pVertexInputState = &vertexInputInfo;
            gbPipeInfo.pInputAssemblyState = &ia;
            gbPipeInfo.pViewportState = &vp;
            gbPipeInfo.pRasterizationState = &rs;
            gbPipeInfo.pMultisampleState = &ms;
            gbPipeInfo.pDepthStencilState = &ds;
            gbPipeInfo.pColorBlendState = &cb;
            gbPipeInfo.pDynamicState = &dynamicState;
            // gbuffer.frag reads set 0 as m_pbrMaterialDSL, so the GBuffer
            // variant must use m_gbufferGeomLayout, never pipelineLay.
            gbPipeInfo.layout = m_gbufferGeomLayout;
            gbPipeInfo.renderPass = m_gbufferRenderPass;
            gbPipeInfo.subpass = 0;

            VkResult gbResult = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &gbPipeInfo, nullptr, &pipes.gbuffer);
            delete gbufShader;

            if (gbResult != VK_SUCCESS) {
                SLEAK_ERROR("VulkanRenderer: Failed to create GBuffer pipeline for vertex format {}",
                            format);
                pipes.gbuffer = VK_NULL_HANDLE;
                pipes.gbufferFailed = true;
            } else {
                SLEAK_INFO("VulkanRenderer: Custom format {} GBuffer pipeline created", format);
            }
        }
    }

    return pipes.main != VK_NULL_HANDLE;
}


/// Binds the custom-format pipeline matching the currently active render pass.
void VulkanRenderer::BeginCustomFormatPass(VertexFormatHandle format) {
    if (!bFrameStarted) return;
    if (format == 0) return;
    if (m_activeCustomFormat == format) return;
    m_activeCustomFormat = format;

    if (!CreateCustomFormatPipelines(format)) return;
    const CustomFormatPipelines& pipes = m_customFormatPipelines[format];

    if (m_shadowPassActive) {
        if (pipes.shadow != VK_NULL_HANDLE) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipes.shadow);
        }
    } else if (m_inGeometryPass && pipes.gbuffer != VK_NULL_HANDLE) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipes.gbuffer);
    } else if (m_inForwardTransparentPass && m_waterPipeline != VK_NULL_HANDLE) {
        // Forward transparent custom-format draws are water — keep the water pipeline
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_waterPipeline);
    } else {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.main);
    }
}


/// Restores the previous pipeline and descriptor set after custom-format draws.
void VulkanRenderer::EndCustomFormatPass() {
    if (!bFrameStarted) return;
    if (m_activeCustomFormat == 0) return;
    m_activeCustomFormat = 0;

    if (m_shadowPassActive) {
        if (m_shadowPipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_shadowPipeline);
        }
    } else if (m_inGeometryPass && m_gbufferPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_gbufferPipeline);
    } else if (m_inForwardTransparentPass) {
        VkPipeline restoreTo = (m_waterPipeline != VK_NULL_HANDLE)
                                   ? m_waterPipeline : pipeline;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, restoreTo);
    } else {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    }

    // Re-bind descriptors after pipeline change; set 0 inside the GBuffer
    // geometry pass belongs to m_gbufferGeomLayout and is owned by BindPBRMaterial.
    if (!m_inGeometryPass && m_textureDescriptorsWritten &&
        CurrentFrameIndex < descriptorSets.size()) {
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLay, 0, 1,
                                &descriptorSets[CurrentFrameIndex], 0, nullptr);
    }
}


/// Destroys every cached custom-format pipeline and clears the map.
void VulkanRenderer::DestroyCustomFormatPipelines() {
    for (auto& entry : m_customFormatPipelines) {
        CustomFormatPipelines& pipes = entry.second;
        if (pipes.main) vkDestroyPipeline(device, pipes.main, nullptr);
        if (pipes.shadow) vkDestroyPipeline(device, pipes.shadow, nullptr);
        if (pipes.gbuffer) vkDestroyPipeline(device, pipes.gbuffer, nullptr);
    }
    m_customFormatPipelines.clear();
    m_activeCustomFormat = 0;
}


/// Destroys only the GBuffer variants; they rebuild lazily against the new GBuffer render pass.
void VulkanRenderer::DestroyCustomFormatGBufferPipelines() {
    for (auto& entry : m_customFormatPipelines) {
        CustomFormatPipelines& pipes = entry.second;
        if (pipes.gbuffer) {
            vkDestroyPipeline(device, pipes.gbuffer, nullptr);
            pipes.gbuffer = VK_NULL_HANDLE;
        }
        pipes.gbufferFailed = false;
    }
    m_activeCustomFormat = 0;
}


/// Binds the debug line pipeline for the current frame.
void VulkanRenderer::BeginDebugLinePass() {
    if (!bFrameStarted) return;
    if (debugLinePipeline == VK_NULL_HANDLE) {
        if (!CreateDebugLinePipeline()) return;
    }
    m_inVoxelPass = false;  // prevent BindVertexBuffer from overriding this pipeline
    m_activeCustomFormat = 0;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      debugLinePipeline);
}


/// Restores the previous pipeline and descriptor set after debug line draws.
void VulkanRenderer::EndDebugLinePass() {
    if (!bFrameStarted) return;

    // Restore pipeline: inside geometry pass restore to the GBuffer pipeline;
    // inside the forward transparent pass restore to water/forward; otherwise main forward.
    VkPipeline restoreTo;
    if (m_inGeometryPass && m_gbufferPipeline != VK_NULL_HANDLE)
        restoreTo = m_gbufferPipeline;
    else if (m_inForwardTransparentPass && m_waterPipeline != VK_NULL_HANDLE)
        restoreTo = m_waterPipeline;
    else
        restoreTo = pipeline;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, restoreTo);

    // Rebind main texture descriptor sets.
    // In the GBuffer geometry pass set 0 belongs to m_gbufferGeomLayout and is
    // managed by BindPBRMaterial — do NOT overwrite it with the forward
    // single-sampler descriptor set or use pipelineLay here.
    if (!m_inGeometryPass && m_textureDescriptorsWritten &&
        CurrentFrameIndex < descriptorSets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &descriptorSets[CurrentFrameIndex], 0, nullptr);
    }
}

/// Compiles the skinned shaders and creates the forward skinned pipeline.
bool VulkanRenderer::CreateSkinnedPipeline() {
    // 1. Compile skinned shaders
    skinnedShader = new VulkanShader(device);
    if (!skinnedShader->compile("assets/shaders/skinned_shader")) {
        SLEAK_ERROR("VulkanRenderer: Failed to compile skinned shaders");
        delete skinnedShader;
        skinnedShader = nullptr;
        return false;
    }

    // 2. Create pipeline (same as main but with skinned shaders)
    VkPipelineShaderStageCreateInfo shaderStages[] = {
        skinnedShader->GetVertexInfo(), skinnedShader->GetFragInfo()};

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Same vertex layout as main pipeline (7 attributes including bone data)
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 7> attributeDescs{};
    attributeDescs[0].binding = 0;
    attributeDescs[0].location = 0;
    attributeDescs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[0].offset = offsetof(Vertex, px);

    attributeDescs[1].binding = 0;
    attributeDescs[1].location = 1;
    attributeDescs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[1].offset = offsetof(Vertex, nx);

    attributeDescs[2].binding = 0;
    attributeDescs[2].location = 2;
    attributeDescs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[2].offset = offsetof(Vertex, tx);

    attributeDescs[3].binding = 0;
    attributeDescs[3].location = 3;
    attributeDescs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[3].offset = offsetof(Vertex, r);

    attributeDescs[4].binding = 0;
    attributeDescs[4].location = 4;
    attributeDescs[4].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescs[4].offset = offsetof(Vertex, u);

    attributeDescs[5].binding = 0;
    attributeDescs[5].location = 5;
    attributeDescs[5].format = VK_FORMAT_R32G32B32A32_SINT;
    attributeDescs[5].offset = offsetof(Vertex, boneIDs);

    attributeDescs[6].binding = 0;
    attributeDescs[6].location = 6;
    attributeDescs[6].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[6].offset = offsetof(Vertex, boneWeights);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
    inputAssemblyInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportInfo{};
    viewportInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.viewportCount = 1;
    viewportInfo.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.sampleShadingEnable = VK_FALSE;
    // When deferred is enabled the skinned pipeline runs inside m_forwardRenderPass
    // which is always 1-sample (GBuffer outputs are resolved separately).
    // When forward-only, match the main render pass sample count.
    msaa.rasterizationSamples = (m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE)
                                ? VK_SAMPLE_COUNT_1_BIT : m_msaaSamples;

    // Skinned: depth test + depth write enabled (same as main pipeline)
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable = VK_FALSE;
    colorBlendInfo.attachmentCount = 1;
    colorBlendInfo.pAttachments = &colorBlendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
    pipelineInfo.pViewportState = &viewportInfo;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &msaa;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlendInfo;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLay;  // Reuse same pipeline layout (set 0 + set 1)
    pipelineInfo.subpass = 0;
    pipelineInfo.renderPass = (m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE)
                              ? m_forwardRenderPass : renderPass;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skinnedPipeline);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to create skinned pipeline!");
        return false;
    }

    SLEAK_INFO("VulkanRenderer: Skinned pipeline created successfully");
    return true;
}


/// Binds the skinned pipeline matching the currently active render pass.
void VulkanRenderer::BeginSkinnedPass() {
    if (!bFrameStarted) return;

    if (m_inGeometryPass) {
        // GBuffer pass — use the GBuffer-compatible skinned pipeline
        if (m_skinnedGbufferPipeline == VK_NULL_HANDLE)
            CreateSkinnedGbufferPipeline();
        if (m_skinnedGbufferPipeline != VK_NULL_HANDLE)
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_skinnedGbufferPipeline);
        return;
    }

    // Forward pass — use the forward skinned pipeline
    if (skinnedPipeline == VK_NULL_HANDLE) {
        if (!CreateSkinnedPipeline()) return;
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipeline);
}


/// Restores the previous pipeline after skinned draws.
void VulkanRenderer::EndSkinnedPass() {
    if (!bFrameStarted) return;

    if (m_inGeometryPass) {
        // Restore static GBuffer pipeline
        if (m_gbufferPipeline != VK_NULL_HANDLE)
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufferPipeline);
        return;
    }

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
}

}
}
