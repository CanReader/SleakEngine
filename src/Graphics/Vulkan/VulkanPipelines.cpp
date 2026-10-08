#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"

#include <Runtime/MeshData.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include "Core/Logger.hpp"

namespace Sleak {
    namespace RenderEngine {

namespace {

// Relative to the working directory, which is the executable's directory
constexpr const char* kPipelineCacheDir = "cache";
constexpr const char* kPipelineCachePath = "cache/vulkan_pipelines.bin";
constexpr const char* kPipelineCacheTempPath = "cache/vulkan_pipelines.bin.tmp";
// VkPipelineCacheHeaderVersionOne: 4 x uint32 + UUID
constexpr size_t kPipelineCacheHeaderSize = 16 + VK_UUID_SIZE;

/// FNV-1a hash, used to skip rewriting an unchanged pipeline cache.
uint64_t HashBytes(const std::vector<char>& data) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : data) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

/// Reads one little-endian field of a pipeline cache header.
uint32_t ReadLE32(const char* bytes) {
    const auto* b = reinterpret_cast<const uint8_t*>(bytes);
    return static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 |
           static_cast<uint32_t>(b[2]) << 16 |
           static_cast<uint32_t>(b[3]) << 24;
}

}  // namespace

/// Creates the pipeline cache, seeded from disk when the saved blob was
/// written by this device and driver.
void VulkanRenderer::CreatePipelineCache() {
    std::vector<char> blob;
    std::ifstream file(kPipelineCachePath, std::ios::binary | std::ios::ate);
    if (file) {
        const std::streamoff size = file.tellg();
        if (size > 0) {
            blob.resize(static_cast<size_t>(size));
            file.seekg(0);
            if (!file.read(blob.data(), size)) blob.clear();
        }
    }
    file.close();

    // Header check
    if (!blob.empty()) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        const bool valid =
            blob.size() >= kPipelineCacheHeaderSize &&
            ReadLE32(blob.data()) >= kPipelineCacheHeaderSize &&
            ReadLE32(blob.data()) <= blob.size() &&
            ReadLE32(blob.data() + 4) ==
                static_cast<uint32_t>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE) &&
            ReadLE32(blob.data() + 8) == props.vendorID &&
            ReadLE32(blob.data() + 12) == props.deviceID &&
            std::memcmp(blob.data() + 16, props.pipelineCacheUUID,
                        VK_UUID_SIZE) == 0;
        if (!valid) {
            SLEAK_INFO(
                "Pipeline cache: ignoring blob from another device or "
                "driver");
            blob.clear();
        }
    }

    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = blob.size();
    info.pInitialData = blob.empty() ? nullptr : blob.data();
    VkResult result =
        vkCreatePipelineCache(device, &info, nullptr, &m_pipelineCache);
    if (result != VK_SUCCESS && !blob.empty()) {
        blob.clear();
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        result =
            vkCreatePipelineCache(device, &info, nullptr, &m_pipelineCache);
    }
    if (result != VK_SUCCESS) {
        m_pipelineCache = VK_NULL_HANDLE;
        SLEAK_WARN(
            "Pipeline cache: creation failed, pipelines compile uncached");
        return;
    }

    m_pipelineCacheLoadedSize = blob.size();
    m_pipelineCacheLoadedHash = blob.empty() ? 0 : HashBytes(blob);
    SLEAK_INFO("Pipeline cache: {} ({} bytes)",
               blob.empty() ? "empty" : "loaded", blob.size());
}

/// Saves the pipeline cache to disk when it changed, then destroys it. Writes
/// a temp file and renames it so a failed write never leaves a torn cache.
void VulkanRenderer::DestroyPipelineCache() {
    if (m_pipelineCache == VK_NULL_HANDLE) return;

    std::vector<char> data;
    size_t size = 0;
    if (vkGetPipelineCacheData(device, m_pipelineCache, &size, nullptr) ==
            VK_SUCCESS &&
        size > 0) {
        data.resize(size);
        if (vkGetPipelineCacheData(device, m_pipelineCache, &size,
                                   data.data()) == VK_SUCCESS)
            data.resize(size);
        else
            data.clear();
    }
    vkDestroyPipelineCache(device, m_pipelineCache, nullptr);
    m_pipelineCache = VK_NULL_HANDLE;

    if (data.empty()) return;
    if (data.size() == m_pipelineCacheLoadedSize &&
        HashBytes(data) == m_pipelineCacheLoadedHash)
        return;

    std::error_code ec;
    std::filesystem::create_directories(kPipelineCacheDir, ec);
    std::ofstream out(kPipelineCacheTempPath,
                      std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
        SLEAK_WARN("Pipeline cache: could not write {}",
                   kPipelineCacheTempPath);
        std::filesystem::remove(kPipelineCacheTempPath, ec);
        return;
    }
    std::filesystem::rename(kPipelineCacheTempPath, kPipelineCachePath, ec);
    if (ec) {
        SLEAK_WARN("Pipeline cache: could not replace {}: {}",
                   kPipelineCachePath, ec.message());
        std::filesystem::remove(kPipelineCacheTempPath, ec);
        return;
    }
    SLEAK_INFO("Pipeline cache: saved {} bytes", data.size());
}

/// Binds the skybox pipeline and its descriptor set for the current frame.
void VulkanRenderer::BeginSkyboxPass() {
    if (!bFrameStarted) return;
    // Skybox only makes sense in forward context — not inside the GBuffer geometry pass
    // where set 0 is a PBR material descriptor set incompatible with pipelineLay.
    if (m_inGeometryPass) return;
    if (skyboxPipeline == VK_NULL_HANDLE || !m_skyboxDescriptorsWritten)
        return;

    m_activeCustomFormat = 0;  // prevent BindVertexBuffer from overriding this pipeline
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
    // otherwise the main forward pipeline.
    VkPipeline restoreTo =
        (m_inGeometryPass && m_gbufferPipeline != VK_NULL_HANDLE)
            ? m_gbufferPipeline : pipeline;
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

/// Creates the layout shared by the forward, skybox, debug line, skinned, and
/// shadow pipelines.
bool VulkanRenderer::CreateMainPipelineLayout() {
    if (pipelineLay != VK_NULL_HANDLE) return true;

    // Push constants
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = 128;  // sizeof(mat4) * 2 = 128 bytes (WVP + World)

    // Descriptor set layouts
    std::array<VkDescriptorSetLayout, 4> setLayouts = {
        descriptorSetLayout, boneDescriptorSetLayout,
        m_lightUBODescriptorSetLayout, m_shadowSamplerDescriptorSetLayout};

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLay) !=
        VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create graphics pipeline layout!");

    return true;
}

/// Creates the main forward graphics pipeline, plus its layout and shader on
/// first use.
bool VulkanRenderer::CreateGraphicsPipeline() {
    if (pipeline != VK_NULL_HANDLE) return true;
    if (!CreateMainPipelineLayout()) return false;

    if (!simpleShader) {
        simpleShader = new VulkanShader(device);
        if (!simpleShader->compile("assets/shaders/default_shader")) {
            delete simpleShader;
            simpleShader = nullptr;
            SLEAK_RETURN_ERR("Cannot compile shaders!")
        }
    }

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

    // Vertex input (locations 0-4)
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 5> attributeDescs{};

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

    VkResult result = vkCreateGraphicsPipelines(
        device, m_pipelineCache, 1, &pipelineInfo, nullptr, &pipeline);
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

/// Compiles the skybox shaders on first use, sizes the skybox descriptor sets
/// to the swapchain image count, and creates the skybox pipeline.
bool VulkanRenderer::CreateSkyboxPipeline() {
    // 1. Compile skybox shaders
    if (!skyboxShader) {
        skyboxShader = new VulkanShader(device);
        if (!skyboxShader->compile("assets/shaders/skybox")) {
            SLEAK_ERROR("VulkanRenderer: Failed to compile skybox shaders");
            delete skyboxShader;
            skyboxShader = nullptr;
            return false;
        }
    }

    // 2. Create skybox descriptor pool and sets (same layout as main)
    uint32_t imageCount =
        static_cast<uint32_t>(swapChainImages.size());

    if (skyboxDescriptorSets.size() != imageCount) {
        if (skyboxDescriptorPool) {
            vkDestroyDescriptorPool(device, skyboxDescriptorPool, nullptr);
            skyboxDescriptorPool = VK_NULL_HANDLE;
        }
        skyboxDescriptorSets.clear();

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
            SLEAK_ERROR(
                "VulkanRenderer: Failed to create skybox descriptor pool");
            return false;
        }

        std::vector<VkDescriptorSetLayout> layouts(imageCount,
                                                   descriptorSetLayout);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = skyboxDescriptorPool;
        allocInfo.descriptorSetCount = imageCount;
        allocInfo.pSetLayouts = layouts.data();

        std::vector<VkDescriptorSet> sets(imageCount);
        if (vkAllocateDescriptorSets(device, &allocInfo, sets.data()) !=
            VK_SUCCESS) {
            SLEAK_ERROR(
                "VulkanRenderer: Failed to allocate skybox descriptor sets");
            return false;
        }
        skyboxDescriptorSets = std::move(sets);
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

    // Same vertex layout as main pipeline (locations 0-4)
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 5> attributeDescs{};
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
        device, m_pipelineCache, 1, &pipelineInfo, nullptr, &skyboxPipeline);
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

    if (!debugLineShader) {
        debugLineShader = new VulkanShader(device);
        if (!debugLineShader->compile("assets/shaders/debug_line")) {
            SLEAK_ERROR("VulkanRenderer: Failed to compile debug line shaders");
            delete debugLineShader;
            debugLineShader = nullptr;
            return false;
        }
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

    std::array<VkVertexInputAttributeDescription, 5> attributeDescs{};
    attributeDescs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)};
    attributeDescs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)};
    attributeDescs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tx)};
    attributeDescs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)};
    attributeDescs[4] = {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)};

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
        device, m_pipelineCache, 1, &pipelineInfo, nullptr, &debugLinePipeline);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to create debug line pipeline!");
        return false;
    }

    SLEAK_INFO("VulkanRenderer: Debug line pipeline created successfully");
    return true;
}


// ============================================================
// Custom vertex format pipelines — vertex input and shader stems
// come from VertexFormatRegistry, so no layout is hard-coded in the backend.
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

/// True when the render pass a custom-format variant targets exists.
bool VulkanRenderer::CustomPassAvailable(CustomPass pass) const {
    switch (pass) {
        case CustomPass::Shadow:
            return m_shadowRenderPass != VK_NULL_HANDLE &&
                   m_shadowShader != nullptr;
        case CustomPass::GBuffer:
            return m_gbufferResourcesCreated &&
                   m_gbufferRenderPass != VK_NULL_HANDLE;
        default:
            return true;
    }
}

/// Returns a format's pipeline for a pass, compiling it on first request.
/// A variant that failed once stays absent instead of recompiling per frame.
VkPipeline VulkanRenderer::GetCustomFormatPipeline(VertexFormatHandle format,
                                                   CustomPass pass) {
    if (format == 0) return VK_NULL_HANDLE;
    if (format > m_customFormatPipelines.size()) {
        if (!VertexFormatRegistry::Get(format)) return VK_NULL_HANDLE;
        m_customFormatPipelines.resize(format);
    }
    CustomFormatPipelines& pipes = m_customFormatPipelines[format - 1];
    const size_t slot = static_cast<size_t>(pass);
    if (pipes.pipelines[slot] != VK_NULL_HANDLE || pipes.failed[slot])
        return pipes.pipelines[slot];

    // Lazy fallback
    if (!CustomPassAvailable(pass)) return VK_NULL_HANDLE;
    const VertexLayoutDesc* desc = VertexFormatRegistry::Get(format);
    if (!desc) return VK_NULL_HANDLE;
    pipes.pipelines[slot] = BuildCustomFormatPipeline(format, *desc, pass);
    pipes.failed[slot] = pipes.pipelines[slot] == VK_NULL_HANDLE;
    return pipes.pipelines[slot];
}

/// Compiles one pipeline variant of a registered layout; returns
/// VK_NULL_HANDLE when its stem is empty or compilation fails.
VkPipeline VulkanRenderer::BuildCustomFormatPipeline(
    VertexFormatHandle format, const VertexLayoutDesc& desc, CustomPass pass) {
    static constexpr const char* kPassNames[CUSTOM_PASS_COUNT] = {
        "main", "shadow", "GBuffer", "transparent"};
    const char* passName = kPassNames[static_cast<size_t>(pass)];

    if (desc.stride == 0 || desc.attributes.empty()) {
        SLEAK_ERROR("VulkanRenderer: Vertex format {} has an empty layout",
                    format);
        return VK_NULL_HANDLE;
    }

    const std::string* stem = &desc.shaderStem;
    if (pass == CustomPass::Shadow) stem = &desc.shadowShaderStem;
    if (pass == CustomPass::GBuffer) stem = &desc.gbufferShaderStem;
    if (pass == CustomPass::Transparent) stem = &desc.transparentShaderStem;
    if (stem->empty()) {
        if (pass == CustomPass::Main)
            SLEAK_ERROR(
                "VulkanRenderer: Vertex format {} has no main shader stem",
                format);
        return VK_NULL_HANDLE;
    }

    // Shader stages
    const std::string base = "assets/shaders/" + *stem;
    std::string shaderPath = base;
    VulkanShader shader(device);
    bool compiled = false;
    if (pass == CustomPass::Shadow) {
        shaderPath = base + ".vert.spv";
        compiled = shader.compileVertexOnly(shaderPath);
    } else if (pass == CustomPass::GBuffer) {
        shaderPath = base + ".vert.spv";
        compiled =
            shader.compile(shaderPath, "assets/shaders/gbuffer.frag.spv");
    } else {
        compiled = shader.compile(base);
    }
    if (!compiled) {
        SLEAK_ERROR(
            "VulkanRenderer: Failed to compile '{}' for vertex format {}",
            shaderPath, format);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[] = {shader.GetVertexInfo(),
                                                shader.GetFragInfo()};

    // Vertex input
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = desc.stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(desc.attributes.size());
    for (const auto& attr : desc.attributes) {
        attributes.push_back(
            {attr.location, 0, ToVkVertexFormat(attr.format), attr.offset});
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = pass == CustomPass::Transparent
                              ? VK_CULL_MODE_NONE
                              : VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (pass == CustomPass::Shadow) {
        rasterizer.depthBiasEnable = VK_TRUE;
        rasterizer.depthBiasConstantFactor = 1.25f;
        rasterizer.depthBiasSlopeFactor = 1.75f;
    }

    // Multisampling
    const bool forwardTarget =
        m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    if (pass == CustomPass::Main ||
        (pass == CustomPass::Transparent && !forwardTarget))
        multisample.rasterizationSamples = m_msaaSamples;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = pass == CustomPass::Shadow
                                      ? VK_COMPARE_OP_LESS_OR_EQUAL
                                      : VK_COMPARE_OP_LESS;

    // Color blending
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (pass == CustomPass::Main || pass == CustomPass::Transparent) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = pass == CustomPass::Transparent
                                        ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                        : VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    std::array<VkPipelineColorBlendAttachmentState, GBUFFER_COUNT> blends;
    blends.fill(blend);

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    if (pass == CustomPass::Shadow) colorBlend.attachmentCount = 0;
    if (pass == CustomPass::GBuffer) colorBlend.attachmentCount = GBUFFER_COUNT;
    colorBlend.pAttachments =
        colorBlend.attachmentCount > 0 ? blends.data() : nullptr;

    // Pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = pass == CustomPass::Shadow ? 1 : 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout =
        pass == CustomPass::GBuffer ? m_gbufferGeomLayout : pipelineLay;
    pipelineInfo.renderPass = forwardTarget ? m_forwardRenderPass : renderPass;
    if (pass == CustomPass::Shadow)
        pipelineInfo.renderPass = m_shadowRenderPass;
    if (pass == CustomPass::GBuffer)
        pipelineInfo.renderPass = m_gbufferRenderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    VkPipeline created = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, m_pipelineCache, 1, &pipelineInfo,
                                  nullptr, &created) != VK_SUCCESS) {
        SLEAK_ERROR(
            "VulkanRenderer: Failed to create {} pipeline for vertex format {}",
            passName, format);
        return VK_NULL_HANDLE;
    }
    SLEAK_INFO("VulkanRenderer: Custom format {} {} pipeline created", format,
               passName);
    return created;
}

/// Compiles the variants the active render path draws with for every
/// registered format not prebuilt yet; the forward main variant is skipped
/// while deferred is active and only built if a draw asks for it.
void VulkanRenderer::PrebuildCustomFormatPipelines() {
    while (VertexFormatRegistry::Get(m_customFormatsPrebuilt + 1)) {
        const VertexFormatHandle format = ++m_customFormatsPrebuilt;
        GetCustomFormatPipeline(format, CustomPass::Shadow);
        GetCustomFormatPipeline(format, IsDeferredEnabled()
                                            ? CustomPass::GBuffer
                                            : CustomPass::Main);
        GetCustomFormatPipeline(format, CustomPass::Transparent);
    }
}


/// Binds the custom-format pipeline matching the currently active render pass.
void VulkanRenderer::BeginCustomFormatPass(VertexFormatHandle format) {
    if (!bFrameStarted) return;
    if (format == 0) return;
    if (m_activeCustomFormat == format) return;
    m_activeCustomFormat = format;

    CustomPass pass = CustomPass::Main;
    if (m_shadowPassActive) {
        pass = CustomPass::Shadow;
    } else if (m_inGeometryPass) {
        pass = CustomPass::GBuffer;
    } else if (m_inForwardTransparentPass) {
        pass = CustomPass::Transparent;
    }

    // No variant for this pass: drop the draws rather than rasterize this
    // layout's vertices through a pipeline built for another one.
    const VkPipeline target = GetCustomFormatPipeline(format, pass);
    m_customFormatUnbound = target == VK_NULL_HANDLE;
    if (m_customFormatUnbound) return;

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, target);
}


/// Restores the previous pipeline and descriptor set after custom-format draws.
void VulkanRenderer::EndCustomFormatPass() {
    if (!bFrameStarted) return;
    if (m_activeCustomFormat == 0) return;
    m_activeCustomFormat = 0;
    m_customFormatUnbound = false;

    if (m_shadowPassActive) {
        if (m_shadowPipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_shadowPipeline);
        }
    } else if (m_inGeometryPass && m_gbufferPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_gbufferPipeline);
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


/// Destroys every cached custom-format pipeline and clears the table.
void VulkanRenderer::DestroyCustomFormatPipelines() {
    for (const CustomFormatPipelines& pipes : m_customFormatPipelines) {
        for (VkPipeline pipe : pipes.pipelines) {
            if (pipe) vkDestroyPipeline(device, pipe, nullptr);
        }
    }
    m_customFormatPipelines.clear();
    m_customFormatsPrebuilt = 0;
    m_activeCustomFormat = 0;
}



/// Binds the debug line pipeline for the current frame.
void VulkanRenderer::BeginDebugLinePass() {
    if (!bFrameStarted) return;
    if (debugLinePipeline == VK_NULL_HANDLE) {
        if (!CreateDebugLinePipeline()) return;
    }
    m_activeCustomFormat = 0;  // prevent BindVertexBuffer from overriding this pipeline
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      debugLinePipeline);
}


/// Restores the previous pipeline and descriptor set after debug line draws.
void VulkanRenderer::EndDebugLinePass() {
    if (!bFrameStarted) return;

    // Restore pipeline: inside geometry pass restore to the GBuffer pipeline;
    // otherwise the main forward pipeline.
    VkPipeline restoreTo =
        (m_inGeometryPass && m_gbufferPipeline != VK_NULL_HANDLE)
            ? m_gbufferPipeline : pipeline;
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

/// Compiles the skinned shaders on first use and creates the forward skinned
/// pipeline.
bool VulkanRenderer::CreateSkinnedPipeline() {
    if (skinnedPipeline != VK_NULL_HANDLE) return true;

    // 1. Compile skinned shaders
    if (!skinnedShader) {
        skinnedShader = new VulkanShader(device);
        if (!skinnedShader->compile("assets/shaders/skinned_shader")) {
            SLEAK_ERROR("VulkanRenderer: Failed to compile skinned shaders");
            delete skinnedShader;
            skinnedShader = nullptr;
            return false;
        }
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
        device, m_pipelineCache, 1, &pipelineInfo, nullptr, &skinnedPipeline);
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
