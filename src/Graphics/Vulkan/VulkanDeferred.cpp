#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanTexture.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanInternal.hpp"

#include <Runtime/MeshData.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include "Core/Logger.hpp"
#include "Camera/Camera.hpp"
#include "Math/Matrix.hpp"

namespace Sleak {
    namespace RenderEngine {

/// GBuffer attachment formats: RT0 AlbedoAO, RT1 NormalRough, RT2 MetalEmit.
const VkFormat VulkanRenderer::m_gbufferFormats[VulkanRenderer::GBUFFER_COUNT] = {
    VK_FORMAT_R8G8B8A8_UNORM,        // RT0: AlbedoAO
    VK_FORMAT_R16G16B16A16_SFLOAT,   // RT1: NormalRough
    VK_FORMAT_R16G16B16A16_SFLOAT,   // RT2: MetalEmit (HDR emissive needs float)
};

/// Creates the GBuffer render pass with its three color attachments and depth.
bool VulkanRenderer::CreateGBufferRenderPass() {
    // Attachment 0-2: GBuffer color RTs (CLEAR → SHADER_READ_ONLY)
    VkAttachmentDescription colorAtts[GBUFFER_COUNT] = {};
    VkAttachmentReference   colorRefs[GBUFFER_COUNT] = {};

    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        colorAtts[i].format         = m_gbufferFormats[i];
        colorAtts[i].samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtts[i].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtts[i].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtts[i].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtts[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtts[i].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtts[i].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        colorRefs[i].attachment = i;
        colorRefs[i].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    // Attachment 3: Depth (CLEAR → DEPTH_STENCIL_READ_ONLY)
    VkAttachmentDescription depthAtt{};
    depthAtt.format         = depthFormat;
    depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = GBUFFER_COUNT;  // depth slot follows the color RTs
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = GBUFFER_COUNT;
    subpass.pColorAttachments       = colorRefs;
    subpass.pDepthStencilAttachment = &depthRef;

    // Two external dependencies:
    // 1. External → subpass (color attachment write)
    // 2. Subpass → external (shader read in lighting pass)
    std::array<VkSubpassDependency, 2> deps{};

    deps[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass      = 0;
    deps[0].srcStageMask    = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask   = VK_ACCESS_MEMORY_READ_BIT;
    deps[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    deps[1].srcSubpass      = 0;
    deps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    std::array<VkAttachmentDescription, GBUFFER_COUNT + 1> attachments;
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) attachments[i] = colorAtts[i];
    attachments[GBUFFER_COUNT] = depthAtt;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpInfo.pAttachments    = attachments.data();
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies   = deps.data();

    return vkCreateRenderPass(device, &rpInfo, nullptr, &m_gbufferRenderPass) == VK_SUCCESS;
}

/// Creates the GBuffer framebuffer binding the GBuffer images and depth.
bool VulkanRenderer::CreateGBufferFramebuffer() {
    std::array<VkImageView, GBUFFER_COUNT + 1> views;
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) views[i] = m_gbufferViews[i];
    views[GBUFFER_COUNT] = depthImageView;

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_gbufferRenderPass;
    fbInfo.attachmentCount = static_cast<uint32_t>(views.size());
    fbInfo.pAttachments    = views.data();
    fbInfo.width           = scExtent.width;
    fbInfo.height          = scExtent.height;
    fbInfo.layers          = 1;

    return vkCreateFramebuffer(device, &fbInfo, nullptr, &m_gbufferFramebuffer) == VK_SUCCESS;
}

/// Compiles the GBuffer shaders and creates the geometry pipeline, reusing pipelineLay.
bool VulkanRenderer::CreateGBufferPipeline() {
    m_gbufferShader = new VulkanShader(device);
    if (!m_gbufferShader->compile("assets/shaders/gbuffer.vert.spv",
                                   "assets/shaders/gbuffer.frag.spv")) {
        SLEAK_ERROR("GBuffer: Failed to compile gbuffer shaders!");
        delete m_gbufferShader;
        m_gbufferShader = nullptr;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[] = {
        m_gbufferShader->GetVertexInfo(),
        m_gbufferShader->GetFragInfo()
    };

    // Dynamic state
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates    = dynamicStates.data();

    // Vertex input — identical to main pipeline
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(Vertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 7> attrDescs{};
    attrDescs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, px)};
    attrDescs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, nx)};
    attrDescs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tx)};
    attrDescs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)};
    attrDescs[4] = {4, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(Vertex, u)};
    attrDescs[5] = {5, 0, VK_FORMAT_R32G32B32A32_SINT,   offsetof(Vertex, boneIDs)};
    attrDescs[6] = {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, boneWeights)};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount   = 1;
    vertexInputInfo.pVertexBindingDescriptions      = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions    = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable        = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth               = 1.0f;
    rasterizer.cullMode                = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable         = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.sampleShadingEnable   = VK_FALSE;
    msaa.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;  // GBuffer is always 1 sample

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable       = VK_TRUE;
    depthStencil.depthWriteEnable      = VK_TRUE;
    depthStencil.depthCompareOp        = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable     = VK_FALSE;

    // GBUFFER_COUNT color blend attachments — opaque, no blending
    VkPipelineColorBlendAttachmentState opaqueBlend{};
    opaqueBlend.blendEnable    = VK_FALSE;
    opaqueBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    std::array<VkPipelineColorBlendAttachmentState, GBUFFER_COUNT> colorBlendAtts;
    colorBlendAtts.fill(opaqueBlend);

    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable   = VK_FALSE;
    colorBlendInfo.attachmentCount = static_cast<uint32_t>(colorBlendAtts.size());
    colorBlendInfo.pAttachments    = colorBlendAtts.data();

    // Use the dedicated GBuffer geometry layout (PBR material DSL at set 0)
    m_gbufferPipelineLayout = m_gbufferGeomLayout;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = shaderStages;
    pipelineInfo.pVertexInputState   = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &msaa;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlendInfo;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_gbufferPipelineLayout;
    pipelineInfo.renderPass          = m_gbufferRenderPass;
    pipelineInfo.subpass             = 0;
    pipelineInfo.basePipelineHandle  = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex   = -1;

    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_gbufferPipeline);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create GBuffer pipeline!");
        return false;
    }

    SLEAK_INFO("VulkanRenderer: GBuffer pipeline created");
    return true;
}

/// Compiles the skinned GBuffer shaders so skinned meshes write into the GBuffer.
bool VulkanRenderer::CreateSkinnedGbufferPipeline() {
    // Load skinned vert + gbuffer frag (SPIR-V already on disk)
    VulkanShader* sh = new VulkanShader(device);
    if (!sh->compile("assets/shaders/skinned_shader.vert.spv",
                      "assets/shaders/gbuffer.frag.spv")) {
        SLEAK_ERROR("GBuffer: Failed to compile skinned gbuffer shaders!");
        delete sh;
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[] = {
        sh->GetVertexInfo(), sh->GetFragInfo()
    };

    std::vector<VkDynamicState> dynStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynState.pDynamicStates    = dynStates.data();

    // 7-attribute vertex layout (identical to static GBuffer pipeline)
    VkVertexInputBindingDescription bindDesc{};
    bindDesc.binding   = 0;
    bindDesc.stride    = sizeof(Vertex);
    bindDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 7> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, px)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, nx)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tx)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)};
    attrs[4] = {4, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(Vertex, u)};
    attrs[5] = {5, 0, VK_FORMAT_R32G32B32A32_SINT,   offsetof(Vertex, boneIDs)};
    attrs[6] = {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, boneWeights)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bindDesc;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions    = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{};
    vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth   = 1.0f;
    rast.cullMode    = VK_CULL_MODE_BACK_BIT;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType               = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState opaqueBlend{};
    opaqueBlend.blendEnable    = VK_FALSE;
    opaqueBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    std::array<VkPipelineColorBlendAttachmentState, GBUFFER_COUNT> blendAtts;
    blendAtts.fill(opaqueBlend);

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = static_cast<uint32_t>(blendAtts.size());
    cb.pAttachments    = blendAtts.data();

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vps;
    pi.pRasterizationState = &rast;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &ds;
    pi.pColorBlendState    = &cb;
    pi.pDynamicState       = &dynState;
    pi.layout              = m_gbufferGeomLayout;
    pi.renderPass          = m_gbufferRenderPass;
    pi.subpass             = 0;
    pi.basePipelineHandle  = VK_NULL_HANDLE;
    pi.basePipelineIndex   = -1;

    VkResult res = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_skinnedGbufferPipeline);
    delete sh;
    if (res != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create skinned GBuffer pipeline!");
        return false;
    }

    SLEAK_INFO("VulkanRenderer: Skinned GBuffer pipeline created");
    return true;
}

/// Creates the deferred lighting render pass with a single color attachment.
bool VulkanRenderer::CreateLightingRenderPass() {
    VkAttachmentDescription colorAtt{};
    // Target the HDR scene color image (R16G16B16A16_SFLOAT) so the lighting
    // pass outputs linear HDR. The composite pass later tonemaps + bloom-blends.
    colorAtt.format         = m_hdrSceneFormat;
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass      = 0;
    deps[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    deps[1].srcSubpass      = 0;
    deps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &colorAtt;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies   = deps.data();

    return vkCreateRenderPass(device, &rpInfo, nullptr, &m_lightingRenderPass) == VK_SUCCESS;
}

/// Creates one lighting pass framebuffer per swapchain image, all aliasing the HDR target.
bool VulkanRenderer::CreateLightingFramebuffers() {
    if (m_hdrSceneView == VK_NULL_HANDLE) {
        SLEAK_ERROR("GBuffer: HDR scene image view is null when creating lighting framebuffers!");
        return false;
    }

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_lightingRenderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments    = &m_hdrSceneView;
    fbInfo.width           = scExtent.width;
    fbInfo.height          = scExtent.height;
    fbInfo.layers          = 1;

    m_lightingFramebuffers.resize(swapChainImageViews.size());
    for (size_t i = 0; i < swapChainImageViews.size(); ++i) {
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_lightingFramebuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("GBuffer: Failed to create lighting framebuffer {}!", i);
            return false;
        }
    }
    return true;
}

/// Compiles the lighting shaders and creates the fullscreen lighting pipeline.
bool VulkanRenderer::CreateLightingPipeline() {
    m_lightingShader = new VulkanShader(device);
    if (!m_lightingShader->compile("assets/shaders/lighting_pass.vert.spv",
                                    "assets/shaders/lighting_pass.frag.spv")) {
        SLEAK_ERROR("GBuffer: Failed to compile lighting pass shaders!");
        delete m_lightingShader;
        m_lightingShader = nullptr;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[] = {
        m_lightingShader->GetVertexInfo(),
        m_lightingShader->GetFragInfo()
    };

    // No vertex input — fullscreen triangle from gl_VertexIndex
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates    = dynamicStates.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable        = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth               = 1.0f;
    rasterizer.cullMode                = VK_CULL_MODE_NONE;
    rasterizer.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable         = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.sampleShadingEnable  = VK_FALSE;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test OFF, depth write OFF
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable   = VK_FALSE;
    depthStencil.depthWriteEnable  = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAtt{};
    colorBlendAtt.blendEnable    = VK_FALSE;
    colorBlendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
    colorBlendInfo.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendInfo.logicOpEnable   = VK_FALSE;
    colorBlendInfo.attachmentCount = 1;
    colorBlendInfo.pAttachments    = &colorBlendAtt;

    // Build lighting pipeline layout:
    // Set 0: m_gbufferSamplerDSL  (7 combined image samplers: GBuffer RTs + shadow maps)
    // Set 1: m_deferredCBDSL      (1 UBO: InvViewProj + screen size)
    // Set 2: m_lightUBODescriptorSetLayout  (1 UBO: directional light + shadow + fog)
    // Set 3: m_iblDSL             (3 samplerCubes + 1 sampler2D + 1 UBO: IBL)
    std::array<VkDescriptorSetLayout, 4> setLayouts = {
        m_gbufferSamplerDSL,
        m_deferredCBDSL,
        m_lightUBODescriptorSetLayout,
        m_iblDSL
    };

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts    = setLayouts.data();

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_lightingPipelineLayout) != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create lighting pipeline layout!");
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = shaderStages;
    pipelineInfo.pVertexInputState   = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &msaa;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlendInfo;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_lightingPipelineLayout;
    pipelineInfo.renderPass          = m_lightingRenderPass;
    pipelineInfo.subpass             = 0;
    pipelineInfo.basePipelineHandle  = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex   = -1;

    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_lightingPipeline);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create lighting pipeline!");
        return false;
    }

    SLEAK_INFO("VulkanRenderer: Lighting pass pipeline created");
    return true;
}

/// Creates the forward transparent render pass writing into the HDR scene image.
bool VulkanRenderer::CreateForwardRenderPass() {
    VkAttachmentDescription colorAtt{};
    colorAtt.format         = m_hdrSceneFormat;
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAtt{};
    depthAtt.format         = depthFormat;
    depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {colorAtt, depthAtt};

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpInfo.pAttachments    = attachments.data();
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies   = &dep;

    return vkCreateRenderPass(device, &rpInfo, nullptr, &m_forwardRenderPass) == VK_SUCCESS;
}

/// Creates one forward transparent framebuffer per swapchain image, all aliasing the HDR target.
bool VulkanRenderer::CreateForwardFramebuffers() {
    if (m_hdrSceneView == VK_NULL_HANDLE) {
        SLEAK_ERROR("GBuffer: HDR scene image view is null when creating forward framebuffers!");
        return false;
    }

    m_forwardFramebuffers.resize(swapChainImageViews.size());

    for (size_t i = 0; i < swapChainImageViews.size(); ++i) {
        std::array<VkImageView, 2> views = {m_hdrSceneView, depthImageView};

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = m_forwardRenderPass;
        fbInfo.attachmentCount = static_cast<uint32_t>(views.size());
        fbInfo.pAttachments    = views.data();
        fbInfo.width           = scExtent.width;
        fbInfo.height          = scExtent.height;
        fbInfo.layers          = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_forwardFramebuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("GBuffer: Failed to create forward framebuffer {}!", i);
            return false;
        }
    }
    return true;
}

/// Creates the GBuffer sampler descriptor set layout, pool, and per-frame sets.
bool VulkanRenderer::CreateGBufferDescriptorSets() {
    // Create GBuffer sampler (nearest for encoded data reads in lighting pass)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_NEAREST;
    samplerInfo.minFilter    = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod       = 0.0f;
    samplerInfo.maxLod       = 1.0f;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_gbufferSampler) != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create gbuffer sampler!");
        return false;
    }

    // Depth sampler — nearest, no comparison (we read raw depth to reconstruct position)
    VkSamplerCreateInfo depthSamplerInfo{};
    depthSamplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    depthSamplerInfo.magFilter    = VK_FILTER_NEAREST;
    depthSamplerInfo.minFilter    = VK_FILTER_NEAREST;
    depthSamplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    depthSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depthSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depthSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depthSamplerInfo.minLod       = 0.0f;
    depthSamplerInfo.maxLod       = 1.0f;

    if (vkCreateSampler(device, &depthSamplerInfo, nullptr, &m_depthSampler) != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create depth sampler!");
        return false;
    }

    // DSL for set 0 of lighting pass:
    // binding 0: RT0, 1: RT1, 2: RT2, 3: depth,
    // binding 4: shadow compare sampler (hardware PCF),
    // binding 5: shadow raw sampler    (PCSS blocker search)
    // binding 6: screen-space AO       (R8, bilateral blurred)
    // (world position is reconstructed from depth binding 3 + InvViewProj)
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    for (uint32_t b = 0; b < 7; ++b) {
        bindings[b].binding         = b;
        bindings[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    dslInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &m_gbufferSamplerDSL) != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create gbuffer sampler DSL!");
        return false;
    }

    // Pool: 7 samplers × MAX_FRAMES_IN_FLIGHT sets
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 7 * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = MAX_FRAMES_IN_FLIGHT;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_gbufferSamplerPool) != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create gbuffer sampler pool!");
        return false;
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_gbufferSamplerDSL);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_gbufferSamplerPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts        = layouts.data();

    if (vkAllocateDescriptorSets(device, &allocInfo, m_gbufferSamplerSets.data()) != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to allocate gbuffer sampler descriptor sets!");
        return false;
    }

    // Initial write — no rendering is in flight yet, so update all frames.
    // After this, per-frame updates happen in UpdateGBufferDescriptors().
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        uint32_t saved = currentFrame;
        currentFrame = f;
        UpdateGBufferDescriptors();
        currentFrame = saved;
    }

    return true;
}

/// Writes the GBuffer, depth, and shadow images into the sampler descriptor sets before the lighting pass.
void VulkanRenderer::UpdateGBufferDescriptors() {
    // Only update the descriptor set for the current frame slot.
    // BeginRender() already waited on this frame's fence, so its
    // descriptor set is safe to update.  Updating other slots would
    // race with the GPU still consuming them.
    uint32_t f = currentFrame;
    std::array<VkDescriptorImageInfo, 7> imageInfos{};

    // RT0..RT2 (AlbedoAO, NormalRough, MetalEmit)
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[i].imageView   = m_gbufferViews[i];
        imageInfos[i].sampler     = m_gbufferSampler;
    }

    // Depth (binding 3) — used to reconstruct world position
    imageInfos[3].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    imageInfos[3].imageView   = depthImageView;
    imageInfos[3].sampler     = m_depthSampler;

    // Shadow map (binding 4) — compare sampler for hardware PCF.
    // Depth images must use DEPTH_STENCIL_READ_ONLY_OPTIMAL (not SHADER_READ_ONLY_OPTIMAL)
    // when accessed as a sampler; using the wrong layout causes VK_ERROR_DEVICE_LOST.
    // Fall back to the default 1x1 white texture when no shadow map is available.
    if (m_shadowImageView) {
        imageInfos[4].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        imageInfos[4].imageView   = m_shadowImageView;
        imageInfos[4].sampler     = m_shadowSampler ? m_shadowSampler
                                                     : (m_defaultTexture ? m_defaultTexture->GetSampler() : VK_NULL_HANDLE);
    } else if (m_defaultTexture) {
        imageInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[4].imageView   = m_defaultTexture->GetImageView();
        imageInfos[4].sampler     = m_defaultTexture->GetSampler();
    }

    // Shadow map raw (binding 5) — non-compare sampler for PCSS blocker search
    if (m_shadowImageView) {
        imageInfos[5].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        imageInfos[5].imageView   = m_shadowImageView;
        imageInfos[5].sampler     = m_shadowRawSampler ? m_shadowRawSampler
                                                        : (m_defaultTexture ? m_defaultTexture->GetSampler() : VK_NULL_HANDLE);
    } else if (m_defaultTexture) {
        imageInfos[5].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[5].imageView   = m_defaultTexture->GetImageView();
        imageInfos[5].sampler     = m_defaultTexture->GetSampler();
    }

    // SSAO (binding 6) — fallback to default white texture when SSAO isn't ready,
    // so the lighting shader multiplies by 1.0 (no occlusion) as a safe default.
    if (m_ssaoBlurView != VK_NULL_HANDLE && m_ssaoSampler != VK_NULL_HANDLE) {
        imageInfos[6].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[6].imageView   = m_ssaoBlurView;
        imageInfos[6].sampler     = m_ssaoSampler;
    } else if (m_defaultTexture) {
        imageInfos[6].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[6].imageView   = m_defaultTexture->GetImageView();
        imageInfos[6].sampler     = m_defaultTexture->GetSampler();
    }

    std::array<VkWriteDescriptorSet, 7> writes{};
    for (uint32_t b = 0; b < 7; ++b) {
        writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet          = m_gbufferSamplerSets[f];
        writes[b].dstBinding      = b;
        writes[b].dstArrayElement = 0;
        writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[b].descriptorCount = 1;
        writes[b].pImageInfo      = &imageInfos[b];
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

/// Creates the per-frame deferred constant buffer holding InvViewProj and screen size.
bool VulkanRenderer::CreateDeferredCBResources() {
    if (m_deferredCBCreated) return true;

    static constexpr VkDeviceSize uboSize = sizeof(DeferredCBData);

    // Per-frame UBO buffers
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size        = uboSize;
        bufInfo.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &bufInfo, nullptr, &m_deferredCBBuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("GBuffer: Failed to create deferred CB buffer!");
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, m_deferredCBBuffers[i], &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &m_deferredCBMemory[i]) != VK_SUCCESS) {
            SLEAK_ERROR("GBuffer: Failed to allocate deferred CB memory!");
            return false;
        }

        vkBindBufferMemory(device, m_deferredCBBuffers[i], m_deferredCBMemory[i], 0);
        vkMapMemory(device, m_deferredCBMemory[i], 0, uboSize, 0, &m_deferredCBMapped[i]);
        memset(m_deferredCBMapped[i], 0, uboSize);
    }

    // DSL: binding 0 = uniform buffer
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding         = 0;
    uboBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings    = &uboBinding;

    if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &m_deferredCBDSL) != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create deferred CB DSL!");
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = MAX_FRAMES_IN_FLIGHT;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_deferredCBPool) != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to create deferred CB pool!");
        return false;
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_deferredCBDSL);

    VkDescriptorSetAllocateInfo dsAllocInfo{};
    dsAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool     = m_deferredCBPool;
    dsAllocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    dsAllocInfo.pSetLayouts        = layouts.data();

    if (vkAllocateDescriptorSets(device, &dsAllocInfo, m_deferredCBSets.data()) != VK_SUCCESS) {
        SLEAK_ERROR("GBuffer: Failed to allocate deferred CB descriptor sets!");
        return false;
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_deferredCBBuffers[i];
        bufInfo.offset = 0;
        bufInfo.range  = uboSize;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_deferredCBSets[i];
        write.dstBinding      = 0;
        write.dstArrayElement = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo     = &bufInfo;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    m_deferredCBCreated = true;
    return true;
}

/// Destroys all GBuffer, lighting, and forward transparent pass resources.
void VulkanRenderer::CleanupGBufferResources() {
    if (!m_gbufferResourcesCreated) return;

    // If a frame is currently being recorded (command buffer is open), wait for
    // the GPU to finish all pending work before destroying resources that may be
    // referenced by the in-flight command buffer.
    if (bFrameStarted) {
        SLEAK_WARN("CleanupGBufferResources called while frame recording is active — forcing device idle");
        vkDeviceWaitIdle(device);
        bFrameStarted = false;
    }

    // GBuffer pipeline
    if (m_gbufferPipeline) {
        vkDestroyPipeline(device, m_gbufferPipeline, nullptr);
        m_gbufferPipeline = VK_NULL_HANDLE;
    }
    if (m_gbufferVoxelPipeline) {
        vkDestroyPipeline(device, m_gbufferVoxelPipeline, nullptr);
        m_gbufferVoxelPipeline = VK_NULL_HANDLE;
    }
    if (m_skinnedGbufferPipeline) {
        vkDestroyPipeline(device, m_skinnedGbufferPipeline, nullptr);
        m_skinnedGbufferPipeline = VK_NULL_HANDLE;
    }
    // m_gbufferPipelineLayout aliases m_gbufferGeomLayout — cleaned up below
    m_gbufferPipelineLayout = VK_NULL_HANDLE;
    delete m_gbufferShader;
    m_gbufferShader = nullptr;

    // Lighting pipeline
    if (m_lightingPipeline) {
        vkDestroyPipeline(device, m_lightingPipeline, nullptr);
        m_lightingPipeline = VK_NULL_HANDLE;
    }
    if (m_lightingPipelineLayout) {
        vkDestroyPipelineLayout(device, m_lightingPipelineLayout, nullptr);
        m_lightingPipelineLayout = VK_NULL_HANDLE;
    }
    delete m_lightingShader;
    m_lightingShader = nullptr;

    // Lighting framebuffers
    for (auto& fb : m_lightingFramebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    m_lightingFramebuffers.clear();

    // Lighting render pass
    if (m_lightingRenderPass) {
        vkDestroyRenderPass(device, m_lightingRenderPass, nullptr);
        m_lightingRenderPass = VK_NULL_HANDLE;
    }

    // GBuffer framebuffer
    if (m_gbufferFramebuffer) {
        vkDestroyFramebuffer(device, m_gbufferFramebuffer, nullptr);
        m_gbufferFramebuffer = VK_NULL_HANDLE;
    }

    // GBuffer render pass
    if (m_gbufferRenderPass) {
        vkDestroyRenderPass(device, m_gbufferRenderPass, nullptr);
        m_gbufferRenderPass = VK_NULL_HANDLE;
    }

    // Forward framebuffers
    for (auto& fb : m_forwardFramebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    m_forwardFramebuffers.clear();

    // Forward render pass
    if (m_forwardRenderPass) {
        vkDestroyRenderPass(device, m_forwardRenderPass, nullptr);
        m_forwardRenderPass = VK_NULL_HANDLE;
    }

    // GBuffer sampler descriptor resources
    if (m_gbufferSamplerPool) {
        vkDestroyDescriptorPool(device, m_gbufferSamplerPool, nullptr);
        m_gbufferSamplerPool = VK_NULL_HANDLE;
    }
    if (m_gbufferSamplerDSL) {
        vkDestroyDescriptorSetLayout(device, m_gbufferSamplerDSL, nullptr);
        m_gbufferSamplerDSL = VK_NULL_HANDLE;
    }
    if (m_gbufferSampler) {
        vkDestroySampler(device, m_gbufferSampler, nullptr);
        m_gbufferSampler = VK_NULL_HANDLE;
    }
    if (m_depthSampler) {
        vkDestroySampler(device, m_depthSampler, nullptr);
        m_depthSampler = VK_NULL_HANDLE;
    }

    // Deferred CB resources
    if (m_deferredCBCreated) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (m_deferredCBMapped[i]) {
                vkUnmapMemory(device, m_deferredCBMemory[i]);
                m_deferredCBMapped[i] = nullptr;
            }
            if (m_deferredCBBuffers[i]) {
                vkDestroyBuffer(device, m_deferredCBBuffers[i], nullptr);
                m_deferredCBBuffers[i] = VK_NULL_HANDLE;
            }
            if (m_deferredCBMemory[i]) {
                vkFreeMemory(device, m_deferredCBMemory[i], nullptr);
                m_deferredCBMemory[i] = VK_NULL_HANDLE;
            }
        }
        m_deferredCBCreated = false;
    }
    if (m_deferredCBPool) {
        vkDestroyDescriptorPool(device, m_deferredCBPool, nullptr);
        m_deferredCBPool = VK_NULL_HANDLE;
    }
    if (m_deferredCBDSL) {
        vkDestroyDescriptorSetLayout(device, m_deferredCBDSL, nullptr);
        m_deferredCBDSL = VK_NULL_HANDLE;
    }

    // GBuffer images
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        if (m_gbufferViews[i]) {
            vkDestroyImageView(device, m_gbufferViews[i], nullptr);
            m_gbufferViews[i] = VK_NULL_HANDLE;
        }
        if (m_gbufferImages[i]) {
            vkDestroyImage(device, m_gbufferImages[i], nullptr);
            m_gbufferImages[i] = VK_NULL_HANDLE;
        }
        if (m_gbufferMemory[i]) {
            vkFreeMemory(device, m_gbufferMemory[i], nullptr);
            m_gbufferMemory[i] = VK_NULL_HANDLE;
        }
    }

    // PBR material resources (GBuffer set 0)
    if (m_pbrMaterialResourcesCreated) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (m_pbrMaterialCBMapped[i]) {
                vkUnmapMemory(device, m_pbrMaterialCBMemory[i]);
                m_pbrMaterialCBMapped[i] = nullptr;
            }
            if (m_pbrMaterialCBBuffers[i]) {
                vkDestroyBuffer(device, m_pbrMaterialCBBuffers[i], nullptr);
                m_pbrMaterialCBBuffers[i] = VK_NULL_HANDLE;
            }
            if (m_pbrMaterialCBMemory[i]) {
                vkFreeMemory(device, m_pbrMaterialCBMemory[i], nullptr);
                m_pbrMaterialCBMemory[i] = VK_NULL_HANDLE;
            }
        }
        m_pbrMaterialResourcesCreated = false;
    }
    if (m_pbrMaterialPool) {
        vkDestroyDescriptorPool(device, m_pbrMaterialPool, nullptr);
        m_pbrMaterialPool = VK_NULL_HANDLE;
    }
    if (m_pbrMaterialDSL) {
        vkDestroyDescriptorSetLayout(device, m_pbrMaterialDSL, nullptr);
        m_pbrMaterialDSL = VK_NULL_HANDLE;
    }
    if (m_gbufferGeomLayout) {
        vkDestroyPipelineLayout(device, m_gbufferGeomLayout, nullptr);
        m_gbufferGeomLayout = VK_NULL_HANDLE;
    }

    // IBL resources
    CleanupIBLResources();

    // SSAO + SSR + Bloom + HDR scene (owned by the deferred pipeline)
    CleanupSSAOResources();
    CleanupSSRResources();
    CleanupBloomResources();

    m_gbufferResourcesCreated    = false;
    m_inGeometryPass             = false;
    m_inForwardTransparentPass   = false;
}

/// Binds the GBuffer pipeline and marks the geometry pass active.
void VulkanRenderer::BindGBufferShader() {
    if (!bFrameStarted || !m_gbufferResourcesCreated) return;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufferPipeline);
    // Reset voxel flag — this binds the default GBuffer pipeline (96-byte Vertex stride).
    // Without this reset, the next voxel draw's BindVertexBuffer sees m_inVoxelPass==true,
    // skips switching to the voxel pipeline, and draws 48-byte VoxelVertex data with
    // a 96-byte stride → corruption.
    m_inVoxelPass = false;
}

/// Runs the deferred lighting pass, reading the GBuffer and writing the HDR scene image.
void VulkanRenderer::ExecuteDeferredLightingPass() {
    if (!bFrameStarted || !m_gbufferResourcesCreated) return;

    // 1. End GBuffer render pass — transitions color RTs → SHADER_READ_ONLY,
    //    depth → DEPTH_STENCIL_READ_ONLY via finalLayout in CreateGBufferRenderPass
    vkCmdEndRenderPass(command);
    m_inGeometryPass = false;
    m_inVoxelPass = false;  // geometry pass is over; pipeline state doesn't survive across render passes

    // 2. Run SSAO (raw + bilateral blur) using GBuffer normal + depth.
    //    This writes to m_ssaoBlurImage which the lighting pass binding 7 reads.
    RenderSSAOPasses();

    // 3. Update GBuffer sampler descriptor sets for current frame
    UpdateGBufferDescriptors();

    // 4. Begin lighting render pass
    VkClearValue clearVal{};
    clearVal.color = {0.0f, 0.0f, 0.0f, 1.0f};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_lightingRenderPass;
    rpBegin.framebuffer       = m_lightingFramebuffers[CurrentFrameIndex];
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = scExtent;
    rpBegin.clearValueCount   = 1;
    rpBegin.pClearValues      = &clearVal;

    vkCmdBeginRenderPass(command, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // 4. Set viewport and scissor
    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(scExtent.width);
    viewport.height   = static_cast<float>(scExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = scExtent;
    vkCmdSetScissor(command, 0, 1, &scissor);

    // 5. Bind lighting pipeline
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipeline);

    // 6. Bind descriptor sets:
    //    set 0: GBuffer samplers (RTs + shadow maps)
    //    set 1: DeferredCB (InvViewProj + screen size)
    //    set 2: LightUBO (directional light + shadow params + fog)
    //    set 3: IBL (irradiance + prefilter + BRDF LUT + settings)
    VkDescriptorSet lightingSets[4] = {
        m_gbufferSamplerSets[currentFrame],
        m_deferredCBSets[currentFrame],
        m_lightUBODescriptorSets[currentFrame],
        m_iblSets[currentFrame]
    };
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_lightingPipelineLayout, 0, 4,
                            lightingSets, 0, nullptr);

    // 7. Fullscreen triangle draw (3 vertices, no VBO)
    vkCmdDraw(command, 3, 1, 0, 0);

    // 8. End lighting render pass
    vkCmdEndRenderPass(command);

    // 9. Transition depth back to DEPTH_STENCIL_ATTACHMENT_OPTIMAL for forward pass
    VkImageMemoryBarrier depthBarrier{};
    depthBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthBarrier.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthBarrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    depthBarrier.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    depthBarrier.image               = depthImage;
    depthBarrier.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(command,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
}

/// Begins the forward transparent render pass over the HDR scene image.
void VulkanRenderer::BeginForwardTransparentPass() {
    if (!bFrameStarted || !m_gbufferResourcesCreated) return;

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_forwardRenderPass;
    rpBegin.framebuffer       = m_forwardFramebuffers[CurrentFrameIndex];
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = scExtent;
    rpBegin.clearValueCount   = 0;  // LOAD_OP — no clear needed

    vkCmdBeginRenderPass(command, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(scExtent.width);
    viewport.height   = static_cast<float>(scExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = scExtent;
    vkCmdSetScissor(command, 0, 1, &scissor);

    // Bind water pipeline when available (uses water_shader SPIR-V for
    // Gerstner waves, Fresnel, sky reflection, GGX specular, SSS, caustics).
    // Fall back to the default forward pipeline otherwise.
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_waterPipeline != VK_NULL_HANDLE ? m_waterPipeline
                                                        : pipeline);

    // Bind descriptor sets 0-3 (same as normal forward pass)
    if (m_textureDescriptorsWritten && CurrentFrameIndex < descriptorSets.size()) {
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLay, 0, 1,
                                &descriptorSets[CurrentFrameIndex], 0, nullptr);
    }
    if (m_boneUBOCreated) {
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLay, 1, 1,
                                &boneDescriptorSets[currentFrame], 0, nullptr);
    }
    if (m_lightUBOCreated) {
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLay, 2, 1,
                                &m_lightUBODescriptorSets[currentFrame], 0, nullptr);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLay, 3, 1,
                                &m_shadowSamplerDescriptorSets[currentFrame], 0, nullptr);
    }

    m_inForwardTransparentPass = true;
    m_forwardPassOpen = true;
    m_inVoxelPass = false;  // new render pass; voxel pipeline state is stale
}

/// Marks the forward transparent pass ended; EndRender closes the actual render pass.
void VulkanRenderer::EndForwardTransparentPass() {
    m_inForwardTransparentPass = false;
    // m_forwardPassOpen stays true — the RP remains open until EndRender
}

/// Copies deferred CB data into the current frame's UBO and snapshots the camera matrices.
void VulkanRenderer::UpdateDeferredCB(const void* data, uint32_t size) {
    if (!m_deferredCBCreated || !data) return;
    uint32_t copySize = std::min(size, static_cast<uint32_t>(sizeof(DeferredCBData)));
    memcpy(m_deferredCBMapped[currentFrame], data, copySize);

    // Snapshot current camera View / Projection for SSAO/SSR/TAA UBO population.
    const Math::Matrix4& V = Camera::GetMainViewMatrix();
    const Math::Matrix4& P = Camera::GetMainProjectionMatrix();
    memcpy(m_cachedView,       &V(0, 0), sizeof(m_cachedView));
    memcpy(m_cachedProjection, &P(0, 0), sizeof(m_cachedProjection));

    // Snapshot InvViewProj (first mat4 of DeferredCBData) so SSAO/SSR reuse the
    // exact inverse the lighting pass uses to reconstruct world from depth.
    if (copySize >= sizeof(m_cachedInvViewProj))
        memcpy(m_cachedInvViewProj, data, sizeof(m_cachedInvViewProj));

    // Compute sub-pixel Halton jitter for this frame (UV space).
    // Applied to WVP push constants in BindConstantBuffer during the geometry pass
    // so each frame samples a slightly different sub-pixel location — the temporal
    // accumulation in TAA then converges to full-resolution anti-aliased output.
    if (m_taaResourcesCreated && m_taaEnabled && scExtent.width > 0) {
        const uint32_t haltonIdx = static_cast<uint32_t>((m_taaFrameIdx % 8) + 1);
        m_taaJitter[0] = (HaltonSeq(haltonIdx, 2) - 0.5f) / static_cast<float>(scExtent.width);
        m_taaJitter[1] = (HaltonSeq(haltonIdx, 3) - 0.5f) / static_cast<float>(scExtent.height);
    } else {
        m_taaJitter[0] = m_taaJitter[1] = 0.0f;
    }
}

}  // namespace RenderEngine
}  // namespace Sleak
