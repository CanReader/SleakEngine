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

/// Creates the full-res raw and blurred SSAO color images, views, and samplers.
bool VulkanRenderer::CreateSSAOImages() {
    // Full-resolution SSAO — half-res caused a visible seam at the center texel boundary.
    m_ssaoExtent.width  = scExtent.width;
    m_ssaoExtent.height = scExtent.height;

    auto createR8 = [&](VkImage& image, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo info{};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.extent.width  = m_ssaoExtent.width;
        info.extent.height = m_ssaoExtent.height;
        info.extent.depth  = 1;
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.format        = m_ssaoFormat;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // TRANSFER_DST enables vkCmdClearColorImage when SSAO is disabled
        // (the lighting pass always samples the blur image regardless).
        info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                           | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &alloc, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(device, image, mem, 0);

        VkImageViewCreateInfo vinfo{};
        vinfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vinfo.image    = image;
        vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vinfo.format   = m_ssaoFormat;
        vinfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(device, &vinfo, nullptr, &view) == VK_SUCCESS;
    };

    if (!createR8(m_ssaoRawImage,  m_ssaoRawMemory,  m_ssaoRawView))  return false;
    if (!createR8(m_ssaoBlurImage, m_ssaoBlurMemory, m_ssaoBlurView)) return false;

    // Linear clamp sampler used by all SSAO consumers (the lighting pass
    // samples at full res — linear reconstructs the half-res buffer smoothly).
    VkSamplerCreateInfo ls{};
    ls.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ls.magFilter    = VK_FILTER_LINEAR;
    ls.minFilter    = VK_FILTER_LINEAR;
    ls.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ls.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ls.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ls.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ls.minLod       = 0.0f;
    ls.maxLod       = 0.0f;
    if (vkCreateSampler(device, &ls, nullptr, &m_ssaoSampler) != VK_SUCCESS) return false;

    // Point sampler for depth input (we want nearest to avoid bilinear
    // bleed across silhouettes when reading the depth buffer).
    VkSamplerCreateInfo ps{};
    ps.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ps.magFilter    = VK_FILTER_NEAREST;
    ps.minFilter    = VK_FILTER_NEAREST;
    ps.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ps.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.minLod       = 0.0f;
    ps.maxLod       = 0.0f;
    if (vkCreateSampler(device, &ps, nullptr, &m_ssaoPointSampler) != VK_SUCCESS) return false;

    return true;
}

/// Creates the shared SSAO render pass (R8 color, DONT_CARE load, shader-read-only output).
bool VulkanRenderer::CreateSSAORenderPass() {
    // Single-attachment render pass — R8 color, DONT_CARE load, STORE out,
    // finalLayout SHADER_READ_ONLY so the next pass can sample directly.
    VkAttachmentDescription colorAtt{};
    colorAtt.format         = m_ssaoFormat;
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
    deps[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments    = &colorAtt;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &subpass;
    rp.dependencyCount = static_cast<uint32_t>(deps.size());
    rp.pDependencies   = deps.data();

    return vkCreateRenderPass(device, &rp, nullptr, &m_ssaoRenderPass) == VK_SUCCESS;
}

/// Creates the raw and blur SSAO framebuffers.
bool VulkanRenderer::CreateSSAOFramebuffers() {
    auto makeFB = [&](VkImageView v, VkFramebuffer& out) -> bool {
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = m_ssaoRenderPass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &v;
        fb.width           = m_ssaoExtent.width;
        fb.height          = m_ssaoExtent.height;
        fb.layers          = 1;
        return vkCreateFramebuffer(device, &fb, nullptr, &out) == VK_SUCCESS;
    };
    if (!makeFB(m_ssaoRawView,  m_ssaoRawFramebuffer))  return false;
    if (!makeFB(m_ssaoBlurView, m_ssaoBlurFramebuffer)) return false;
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
    if (vkCreateImage(device, &info, nullptr, &m_ssaoNoiseImage) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, m_ssaoNoiseImage, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &m_ssaoNoiseMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, m_ssaoNoiseImage, m_ssaoNoiseMemory, 0);

    VkImageViewCreateInfo vinfo{};
    vinfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vinfo.image    = m_ssaoNoiseImage;
    vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vinfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
    vinfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vinfo, nullptr, &m_ssaoNoiseView) != VK_SUCCESS) return false;

    // Staging + upload.
    VkBuffer       staging       = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    const VkDeviceSize uploadSize = pixels.size();

    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = uploadSize;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, nullptr, &staging) != VK_SUCCESS) return false;

    VkMemoryRequirements sreq;
    vkGetBufferMemoryRequirements(device, staging, &sreq);
    VkMemoryAllocateInfo salloc{};
    salloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    salloc.allocationSize  = sreq.size;
    salloc.memoryTypeIndex = FindMemoryType(sreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &salloc, nullptr, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, staging, nullptr);
        return false;
    }
    vkBindBufferMemory(device, staging, stagingMemory, 0);

    void* mapped = nullptr;
    vkMapMemory(device, stagingMemory, 0, uploadSize, 0, &mapped);
    memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(device, stagingMemory);

    // Single-shot command buffer for upload.
    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandPool        = commands;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cbAlloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier b0{};
    b0.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b0.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    b0.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b0.image               = m_ssaoNoiseImage;
    b0.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b0.srcAccessMask       = 0;
    b0.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b0);

    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset       = {0, 0, 0};
    region.imageExtent       = {SSAO_NOISE_SIZE, SSAO_NOISE_SIZE, 1};
    vkCmdCopyBufferToImage(cmd, staging, m_ssaoNoiseImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier b1 = b0;
    b1.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b1);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(device, commands, 1, &cmd);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

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
    gpi.renderPass          = m_ssaoRenderPass;
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
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_ssaoBlurPipeline) != VK_SUCCESS) {
        SLEAK_ERROR("SSAO: failed to create ssao blur pipeline");
        return false;
    }

    return true;
}

/// Creates all SSAO images, render pass, framebuffers, descriptors, and pipelines.
bool VulkanRenderer::CreateSSAOResources() {
    if (m_ssaoResourcesCreated) return true;

    if (!CreateSSAOImages())               { SLEAK_ERROR("SSAO: images failed");        return false; }
    if (!CreateSSAONoiseTexture())         { SLEAK_ERROR("SSAO: noise failed");         return false; }
    if (!CreateSSAORenderPass())           { SLEAK_ERROR("SSAO: render pass failed");   return false; }
    if (!CreateSSAOFramebuffers())         { SLEAK_ERROR("SSAO: framebuffers failed");  return false; }
    if (!CreateSSAODescriptorResources())  { SLEAK_ERROR("SSAO: descriptors failed");   return false; }
    if (!CreateSSAOPipelines())            { SLEAK_ERROR("SSAO: pipelines failed");     return false; }

    m_ssaoResourcesCreated = true;
    SLEAK_INFO("SSAO resources created ({}x{})", m_ssaoExtent.width, m_ssaoExtent.height);
    return true;
}

/// Destroys all SSAO pipelines, framebuffers, descriptors, images, and samplers.
void VulkanRenderer::CleanupSSAOResources() {
    if (!m_ssaoResourcesCreated) return;

    if (m_ssaoPipeline)              { vkDestroyPipeline(device, m_ssaoPipeline, nullptr);              m_ssaoPipeline = VK_NULL_HANDLE; }
    if (m_ssaoBlurPipeline)          { vkDestroyPipeline(device, m_ssaoBlurPipeline, nullptr);          m_ssaoBlurPipeline = VK_NULL_HANDLE; }
    if (m_ssaoPipelineLayout)        { vkDestroyPipelineLayout(device, m_ssaoPipelineLayout, nullptr);  m_ssaoPipelineLayout = VK_NULL_HANDLE; }
    if (m_ssaoBlurPipelineLayout)    { vkDestroyPipelineLayout(device, m_ssaoBlurPipelineLayout, nullptr); m_ssaoBlurPipelineLayout = VK_NULL_HANDLE; }
    delete m_ssaoShader;     m_ssaoShader     = nullptr;
    delete m_ssaoBlurShader; m_ssaoBlurShader = nullptr;

    if (m_ssaoRawFramebuffer)  { vkDestroyFramebuffer(device, m_ssaoRawFramebuffer, nullptr);  m_ssaoRawFramebuffer = VK_NULL_HANDLE; }
    if (m_ssaoBlurFramebuffer) { vkDestroyFramebuffer(device, m_ssaoBlurFramebuffer, nullptr); m_ssaoBlurFramebuffer = VK_NULL_HANDLE; }
    if (m_ssaoRenderPass)      { vkDestroyRenderPass(device, m_ssaoRenderPass, nullptr);       m_ssaoRenderPass = VK_NULL_HANDLE; }

    if (m_ssaoDescriptorPool)  { vkDestroyDescriptorPool(device, m_ssaoDescriptorPool, nullptr); m_ssaoDescriptorPool = VK_NULL_HANDLE; }
    if (m_ssaoInputDSL)        { vkDestroyDescriptorSetLayout(device, m_ssaoInputDSL, nullptr); m_ssaoInputDSL = VK_NULL_HANDLE; }
    if (m_ssaoUboDSL)          { vkDestroyDescriptorSetLayout(device, m_ssaoUboDSL,   nullptr); m_ssaoUboDSL   = VK_NULL_HANDLE; }
    if (m_ssaoBlurDSL)         { vkDestroyDescriptorSetLayout(device, m_ssaoBlurDSL,  nullptr); m_ssaoBlurDSL  = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_ssaoUboMapped[i])  { vkUnmapMemory(device, m_ssaoUboMemory[i]); m_ssaoUboMapped[i] = nullptr; }
        if (m_ssaoUboBuffers[i]) { vkDestroyBuffer(device, m_ssaoUboBuffers[i], nullptr); m_ssaoUboBuffers[i] = VK_NULL_HANDLE; }
        if (m_ssaoUboMemory[i])  { vkFreeMemory(device, m_ssaoUboMemory[i], nullptr);     m_ssaoUboMemory[i]  = VK_NULL_HANDLE; }
    }

    if (m_ssaoRawView)    { vkDestroyImageView(device, m_ssaoRawView, nullptr);  m_ssaoRawView = VK_NULL_HANDLE; }
    if (m_ssaoBlurView)   { vkDestroyImageView(device, m_ssaoBlurView, nullptr); m_ssaoBlurView = VK_NULL_HANDLE; }
    if (m_ssaoRawImage)   { vkDestroyImage(device, m_ssaoRawImage, nullptr);     m_ssaoRawImage = VK_NULL_HANDLE; }
    if (m_ssaoBlurImage)  { vkDestroyImage(device, m_ssaoBlurImage, nullptr);    m_ssaoBlurImage = VK_NULL_HANDLE; }
    if (m_ssaoRawMemory)  { vkFreeMemory(device, m_ssaoRawMemory, nullptr);      m_ssaoRawMemory = VK_NULL_HANDLE; }
    if (m_ssaoBlurMemory) { vkFreeMemory(device, m_ssaoBlurMemory, nullptr);     m_ssaoBlurMemory = VK_NULL_HANDLE; }

    if (m_ssaoNoiseView)    { vkDestroyImageView(device, m_ssaoNoiseView, nullptr); m_ssaoNoiseView = VK_NULL_HANDLE; }
    if (m_ssaoNoiseImage)   { vkDestroyImage(device, m_ssaoNoiseImage, nullptr);    m_ssaoNoiseImage = VK_NULL_HANDLE; }
    if (m_ssaoNoiseMemory)  { vkFreeMemory(device, m_ssaoNoiseMemory, nullptr);     m_ssaoNoiseMemory = VK_NULL_HANDLE; }
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
        blurInfos[0].imageView   = m_ssaoRawView;
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

/// Clears the SSAO/SSR/bloom fallback images once so disabled effects sample defined black/white content.
void VulkanRenderer::InitDisabledEffectFallbacks() {
    if (m_ssaoBlurImage == VK_NULL_HANDLE ||
        m_ssrImage == VK_NULL_HANDLE ||
        m_bloomImage == VK_NULL_HANDLE)
        return;

    VkCommandBufferAllocateInfo ca{};
    ca.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandPool        = commands;
    ca.commandBufferCount = 1;
    VkCommandBuffer initCmd;
    if (vkAllocateCommandBuffers(device, &ca, &initCmd) != VK_SUCCESS) return;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(initCmd, &bi);

    const VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    struct Prime { VkImage img; VkClearColorValue clr; };
    VkClearColorValue white{}; white.float32[0] = 1.0f; white.float32[1] = 1.0f;
                               white.float32[2] = 1.0f; white.float32[3] = 1.0f;
    VkClearColorValue black{};
    Prime items[3] = {
        { m_ssaoBlurImage, white },   // white = no occlusion
        { m_ssrImage,      black },   // black = no reflection
        { m_bloomImage,    black },   // black = no bloom (mip 0)
    };

    for (auto& it : items) {
        VkImageMemoryBarrier bar{};
        bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.srcAccessMask       = 0;
        bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.image               = it.img;
        bar.subresourceRange    = sr;
        vkCmdPipelineBarrier(initCmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);

        vkCmdClearColorImage(initCmd, it.img,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &it.clr, 1, &sr);

        bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
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

    m_ssaoFallbackPrimed  = true;
    m_ssrFallbackPrimed   = true;
    m_bloomFallbackPrimed = true;
}

/// Runs the raw SSAO and bilateral blur passes, or clears the blur target when SSAO is disabled.
void VulkanRenderer::RenderSSAOPasses() {
    if (!m_ssaoResourcesCreated) return;

    // SSAO disabled: clear the blur target (sampled by the lighting pass at
    // binding 7) to white = no occlusion, and leave it SHADER_READ_ONLY so the
    // lighting pass never samples an UNDEFINED image. Mirrors the SSR fallback.
    if (!m_ssaoEnabled) {
        // Already primed to white SHADER_READ_ONLY — the content is static, so
        // re-clearing every frame is wasted work. Re-prime only if the enabled
        // path dirtied the image since (runtime toggle).
        if (m_ssaoFallbackPrimed) return;
        VkImageMemoryBarrier toClear{};
        toClear.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toClear.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toClear.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.srcAccessMask       = 0;
        toClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toClear.image               = m_ssaoBlurImage;
        toClear.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toClear);

        VkClearColorValue white{};
        white.float32[0] = 1.0f; white.float32[1] = 1.0f;
        white.float32[2] = 1.0f; white.float32[3] = 1.0f;
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, m_ssaoBlurImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);

        VkImageMemoryBarrier toRead = toClear;
        toRead.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toRead);
        m_ssaoFallbackPrimed = true;
        return;
    }

    // Enabled path dirties the blur image; force a re-prime if SSAO is later
    // disabled so the lighting pass doesn't sample stale occlusion.
    m_ssaoFallbackPrimed = false;

    UpdateSSAOUBO();

    // ---- Pass 1: raw SSAO ----
    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = m_ssaoRenderPass;
    rp.framebuffer       = m_ssaoRawFramebuffer;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = m_ssaoExtent;
    rp.clearValueCount   = 0;

    vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    FillFullscreenViewportScissor(command, m_ssaoExtent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaoPipeline);
    VkDescriptorSet ssaoSets[2] = { m_ssaoInputSets[currentFrame], m_ssaoUboSets[currentFrame] };
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_ssaoPipelineLayout, 0, 2, ssaoSets, 0, nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);

    // ---- Pass 2: bilateral blur ----
    rp.framebuffer = m_ssaoBlurFramebuffer;
    vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    FillFullscreenViewportScissor(command, m_ssaoExtent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaoBlurPipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_ssaoBlurPipelineLayout, 0, 1,
                            &m_ssaoBlurSets[currentFrame], 0, nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
}

}  // namespace RenderEngine
}  // namespace Sleak
