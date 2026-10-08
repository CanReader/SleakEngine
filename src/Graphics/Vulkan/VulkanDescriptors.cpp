#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanTexture.hpp"

#include <Core/Window.hpp>
#include <array>
#include <cstring>
#include <vector>
#include "Core/Logger.hpp"

namespace Sleak {
    namespace RenderEngine {

/// Creates the texture, bone UBO, light UBO, and shadow sampler descriptor set layouts.
bool VulkanRenderer::CreateDescriptorSetLayout() {
    // Set 0: texture sampler
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerBinding;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr,
                                     &descriptorSetLayout) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create descriptor set layout!");

    // Set 1: bone UBO (for skeletal animation)
    VkDescriptorSetLayoutBinding boneUBOBinding{};
    boneUBOBinding.binding = 0;
    boneUBOBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    boneUBOBinding.descriptorCount = 1;
    boneUBOBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    boneUBOBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo boneLayoutInfo{};
    boneLayoutInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    boneLayoutInfo.bindingCount = 1;
    boneLayoutInfo.pBindings = &boneUBOBinding;

    if (vkCreateDescriptorSetLayout(device, &boneLayoutInfo, nullptr,
                                     &boneDescriptorSetLayout) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create bone descriptor set layout!");

    // Set 2: light/shadow UBO
    VkDescriptorSetLayoutBinding lightUBOBinding{};
    lightUBOBinding.binding = 0;
    lightUBOBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightUBOBinding.descriptorCount = 1;
    lightUBOBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    lightUBOBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo lightUBOLayoutInfo{};
    lightUBOLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lightUBOLayoutInfo.bindingCount = 1;
    lightUBOLayoutInfo.pBindings = &lightUBOBinding;

    if (vkCreateDescriptorSetLayout(device, &lightUBOLayoutInfo, nullptr,
                                     &m_lightUBODescriptorSetLayout) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create light UBO descriptor set layout!");

    // Set 3: shadow map samplers — binding 0 = compare sampler (PCF),
    //                              binding 1 = raw sampler (PCSS blocker search)
    std::array<VkDescriptorSetLayoutBinding, 2> shadowSamplerBindings{};
    shadowSamplerBindings[0].binding = 0;
    shadowSamplerBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowSamplerBindings[0].descriptorCount = 1;
    shadowSamplerBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    shadowSamplerBindings[0].pImmutableSamplers = nullptr;

    shadowSamplerBindings[1].binding = 1;
    shadowSamplerBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowSamplerBindings[1].descriptorCount = 1;
    shadowSamplerBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    shadowSamplerBindings[1].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo shadowSamplerLayoutInfo{};
    shadowSamplerLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    shadowSamplerLayoutInfo.bindingCount = static_cast<uint32_t>(shadowSamplerBindings.size());
    shadowSamplerLayoutInfo.pBindings = shadowSamplerBindings.data();

    if (vkCreateDescriptorSetLayout(device, &shadowSamplerLayoutInfo, nullptr,
                                     &m_shadowSamplerDescriptorSetLayout) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create shadow sampler descriptor set layout!");

    return true;
}


/// Creates the descriptor pool backing the per-texture descriptor sets.
bool VulkanRenderer::CreateDescriptorPool() {
    uint32_t imageCount =
        static_cast<uint32_t>(swapChainImages.size());

    // Allow up to 128 textures, each needing imageCount descriptor sets
    static constexpr uint32_t MAX_TEXTURES = 1024;
    uint32_t totalSets = imageCount * MAX_TEXTURES;

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = totalSets;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = totalSets;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr,
                                &descriptorPool) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create descriptor pool!");

    return true;
}


/// Allocates one texture descriptor set per swapchain image.
bool VulkanRenderer::AllocateDescriptorSets() {
    uint32_t imageCount =
        static_cast<uint32_t>(swapChainImages.size());

    std::vector<VkDescriptorSetLayout> layouts(imageCount,
                                                descriptorSetLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = imageCount;
    allocInfo.pSetLayouts = layouts.data();

    descriptorSets.resize(imageCount);
    if (vkAllocateDescriptorSets(device, &allocInfo,
                                  descriptorSets.data()) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to allocate descriptor sets!");

    return true;
}


/// Creates the fallback 1x1 white texture and writes it into the global descriptor sets.
bool VulkanRenderer::CreateDefaultTexture() {
    // Create a 1x1 white pixel texture as fallback so descriptor sets
    // are always valid, even when no user texture is loaded.
    uint32_t whitePixel = 0xFFFFFFFF;  // RGBA(255,255,255,255)
    m_defaultTexture = new VulkanTexture(device, physicalDevice, commands,
                                          graphicsQueue);
    if (!m_defaultTexture->LoadFromMemory(&whitePixel, 1, 1,
                                           TextureFormat::RGBA8)) {
        delete m_defaultTexture;
        m_defaultTexture = nullptr;
        return false;
    }

    // Allocate per-texture descriptor sets for the default texture
    WriteTextureDescriptors(m_defaultTexture);

    // Also write the default texture to the global descriptor sets
    // (used as initial binding in BeginRender)
    for (size_t i = 0; i < descriptorSets.size(); i++) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = m_defaultTexture->GetImageView();
        imageInfo.sampler = m_defaultTexture->GetSampler();

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = descriptorSets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }
    m_textureDescriptorsWritten = true;

    return true;
}


/// Allocates and writes a per-texture descriptor set for the given texture.
void VulkanRenderer::WriteTextureDescriptors(VulkanTexture* texture) {
    if (!texture || texture->GetImageView() == VK_NULL_HANDLE ||
        texture->GetSampler() == VK_NULL_HANDLE)
        return;

    uint32_t imageCount = static_cast<uint32_t>(swapChainImages.size());

    // Allocate per-texture descriptor sets (one per swapchain image)
    std::vector<VkDescriptorSetLayout> layouts(imageCount, descriptorSetLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = imageCount;
    allocInfo.pSetLayouts = layouts.data();

    std::vector<VkDescriptorSet> sets(imageCount);
    if (vkAllocateDescriptorSets(device, &allocInfo, sets.data()) != VK_SUCCESS) {
        SLEAK_ERROR("VulkanRenderer: Failed to allocate descriptor sets for texture");
        return;
    }

    // Write the texture's imageView/sampler to each descriptor set
    for (size_t i = 0; i < sets.size(); i++) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = texture->GetImageView();
        imageInfo.sampler = texture->GetSampler();

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = sets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    texture->SetDescriptorSets(std::move(sets), descriptorPool);
}

/// Initializes ImGui and its Vulkan backend against the active render pass.
bool VulkanRenderer::CreateImGUI() {
    if (!device || !instance || !graphicsQueue)
        return false;

    // In deferred mode, ImGui renders inside the bloom COMPOSITE pass (swapchain
    // target, post-tonemap). Forward pass can't host ImGui because its output
    // is linear HDR and it ends in SHADER_READ_ONLY_OPTIMAL for bloom sampling.
    VkRenderPass imguiRenderPass = VK_NULL_HANDLE;
    if (m_gbufferResourcesCreated && m_deferredEnabled && m_bloomCompositeRenderPass != VK_NULL_HANDLE) {
        imguiRenderPass = m_bloomCompositeRenderPass;
    } else if (m_gbufferResourcesCreated && m_deferredEnabled && m_forwardRenderPass != VK_NULL_HANDLE) {
        imguiRenderPass = m_forwardRenderPass;
    } else {
        imguiRenderPass = renderPass;
    }

    if (!imguiRenderPass)
        return false;

    // ImGui backend allocates split SAMPLED_IMAGE + SAMPLER sets, not COMBINED_IMAGE_SAMPLER
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
        {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 64 + IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr,
                               &imguiDescriptorPool) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create ImGUI descriptor pool!");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(sdlWindow->GetSDLWindow()))
        return false;

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = instance;
    initInfo.PhysicalDevice = physicalDevice;
    initInfo.Device = device;
    initInfo.QueueFamily = QueueIDs.GraphicsIndex;
    initInfo.Queue = graphicsQueue;
    initInfo.PipelineCache = m_pipelineCache;
    initInfo.DescriptorPool = imguiDescriptorPool;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount =
        static_cast<uint32_t>(swapChainImages.size());
    initInfo.PipelineInfoMain.MSAASamples = (m_gbufferResourcesCreated && m_deferredEnabled)
                          ? VK_SAMPLE_COUNT_1_BIT : m_msaaSamples;
    initInfo.PipelineInfoMain.RenderPass = imguiRenderPass;
    initInfo.PipelineInfoMain.Subpass = 0;

    if (!ImGui_ImplVulkan_Init(&initInfo))
        return false;

    bImInitialized = true;
    return true;
}

/// Creates each frame's first bone ring chunk; slot 0 holds identity.
bool VulkanRenderer::CreateBoneUBOResources() {
    if (m_boneUBOCreated) return true;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment;
    m_boneSlotStride = BONE_PALETTE_BYTES;
    if (align > 0)
        m_boneSlotStride = (m_boneSlotStride + align - 1) / align * align;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (!AddBoneUBOChunk(i)) {
            CleanupBoneUBOResources();
            return false;
        }

        // Identity palette in slot 0
        auto* matrices = static_cast<float*>(m_boneChunks[i][0].mapped);
        for (uint32_t b = 0; b < BONE_PALETTE_BYTES / 64; ++b) {
            for (int c = 0; c < 16; ++c)
                matrices[b * 16 + c] = (c % 5 == 0) ? 1.0f : 0.0f;
        }
        m_boneSlot[i] = 1;
    }

    m_boneUBOCreated = true;
    SLEAK_INFO(
        "VulkanRenderer: Bone UBO ring created ({} x {} bytes per frame)",
        BONE_SLOTS_PER_CHUNK, m_boneSlotStride);
    return true;
}

/// Appends BONE_SLOTS_PER_CHUNK palette slots to one frame's bone ring: its
/// own buffer and dynamic-UBO set, so sets bound earlier stay valid.
bool VulkanRenderer::AddBoneUBOChunk(uint32_t frame) {
    BoneUBOChunk chunk{};

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &chunk.pool) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Failed to create bone descriptor pool!");
        return false;
    }

    VkDescriptorSetAllocateInfo dsAllocInfo{};
    dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool = chunk.pool;
    dsAllocInfo.descriptorSetCount = 1;
    dsAllocInfo.pSetLayouts = &boneDescriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &dsAllocInfo, &chunk.set) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate bone descriptor set!");
        vkDestroyDescriptorPool(device, chunk.pool, nullptr);
        return false;
    }

    // Host-visible, coherent for fast CPU writes
    const VkDeviceSize size = m_boneSlotStride * BONE_SLOTS_PER_CHUNK;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &chunk.buffer) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Failed to create bone UBO buffer!");
        vkDestroyDescriptorPool(device, chunk.pool, nullptr);
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, chunk.buffer, &memReqs);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &chunk.memory) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate bone UBO memory!");
        vkDestroyBuffer(device, chunk.buffer, nullptr);
        vkDestroyDescriptorPool(device, chunk.pool, nullptr);
        return false;
    }
    vkBindBufferMemory(device, chunk.buffer, chunk.memory, 0);
    vkMapMemory(device, chunk.memory, 0, size, 0, &chunk.mapped);

    // One palette range; binds select the slot by dynamic offset
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = chunk.buffer;
    bufInfo.offset = 0;
    bufInfo.range = BONE_PALETTE_BYTES;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = chunk.set;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);

    m_boneChunks[frame].push_back(chunk);
    if (m_boneChunks[frame].size() > 1)
        SLEAK_INFO("VulkanRenderer: frame {} bone ring grew to {} slots", frame,
                   m_boneChunks[frame].size() * BONE_SLOTS_PER_CHUNK);
    return true;
}

/// Destroys every bone ring chunk of every frame.
void VulkanRenderer::CleanupBoneUBOResources() {
    for (auto& chunks : m_boneChunks) {
        for (BoneUBOChunk& chunk : chunks) {
            if (chunk.mapped) vkUnmapMemory(device, chunk.memory);
            if (chunk.buffer) vkDestroyBuffer(device, chunk.buffer, nullptr);
            if (chunk.memory) vkFreeMemory(device, chunk.memory, nullptr);
            if (chunk.pool)
                vkDestroyDescriptorPool(device, chunk.pool, nullptr);
        }
        chunks.clear();
    }
    m_boneUBOCreated = false;
}

/// Binds the identity palette (ring slot 0) at set 1.
void VulkanRenderer::BindIdentityBonePalette(VkPipelineLayout layout) {
    if (!m_boneUBOCreated || m_boneChunks[currentFrame].empty()) return;
    const uint32_t offset = 0;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1,
                            1, &m_boneChunks[currentFrame][0].set, 1, &offset);
}

/// Copies bone matrices into this bind's own slot of the frame's bone ring
/// and binds set 1 at that slot, so every skinned draw keeps its palette.
void VulkanRenderer::BindBoneBuffer(RefPtr<BufferBase> buffer) {
    if (!bFrameStarted) return;
    if (!buffer) return;

    // Lazily create bone UBO resources on first use
    if (!m_boneUBOCreated) {
        if (!CreateBoneUBOResources()) return;
    }

    auto* vkBuf = static_cast<VulkanBuffer*>(buffer.get());
    if (!vkBuf) return;

    void* data = vkBuf->GetData();
    if (!data) return;

    uint32_t size = static_cast<uint32_t>(vkBuf->GetSize());
    if (size > BONE_PALETTE_BYTES) size = BONE_PALETTE_BYTES;

    // Claim the next ring slot (use currentFrame, not CurrentFrameIndex
    // which is the swapchain image index and can exceed MAX_FRAMES_IN_FLIGHT)
    const uint32_t slot = m_boneSlot[currentFrame];
    auto& chunks = m_boneChunks[currentFrame];
    const uint32_t chunkIdx = slot / BONE_SLOTS_PER_CHUNK;
    if (chunkIdx >= chunks.size() && !AddBoneUBOChunk(currentFrame)) return;
    const BoneUBOChunk& chunk = chunks[chunkIdx];
    const uint32_t offset =
        static_cast<uint32_t>((slot % BONE_SLOTS_PER_CHUNK) * m_boneSlotStride);
    memcpy(static_cast<char*>(chunk.mapped) + offset, data, size);
    m_boneSlot[currentFrame] = slot + 1;

    // Bind bone descriptor set at set index 1.
    // In the GBuffer geometry pass the active layout is m_gbufferGeomLayout;
    // outside it pipelineLay is active. The layout used here must match the
    // pipeline that will draw, because Vulkan invalidates sets when layouts
    // are incompatible at lower-numbered sets (set 0 differs between the two).
    VkPipelineLayout boneBindLayout = (m_inGeometryPass && m_gbufferGeomLayout != VK_NULL_HANDLE)
                                      ? m_gbufferGeomLayout : pipelineLay;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            boneBindLayout, 1, 1, &chunk.set, 1, &offset);
}

/// Creates the per-frame PBR material descriptor set (set 0 in GBuffer pass):
/// bindings 0-5 are combined image samplers, binding 6 is the params UBO.
/// Also creates m_gbufferGeomLayout used by the GBuffer pipeline.
bool VulkanRenderer::CreatePBRMaterialResources() {
    if (m_pbrMaterialResourcesCreated) return true;

    // --- Descriptor Set Layout ---
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    for (uint32_t i = 0; i < 6; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[6].binding         = 6;
    bindings[6].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 7;
    dslInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &m_pbrMaterialDSL) != VK_SUCCESS) {
        SLEAK_ERROR("PBR: Failed to create PBR material DSL!");
        return false;
    }

    // --- Material params UBO stride, then one starting chunk per frame ---
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    VkDeviceSize minAlign = props.limits.minUniformBufferOffsetAlignment;
    VkDeviceSize stride = sizeof(PBRMaterialParams);
    if (minAlign > 0)
        stride = ((stride + minAlign - 1) / minAlign) * minAlign;
    m_pbrMaterialUBOStride = stride;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (!AddPBRMaterialChunk(i)) return false;
    }

    // --- GBuffer geometry pipeline layout ---
    // Set 0: PBR material DSL, Set 1: bone DSL, Set 2: lightUBO DSL, Set 3: shadow sampler DSL
    // Push constants: VK_SHADER_STAGE_VERTEX_BIT, offset=0, size=128 (WVP + World)
    std::array<VkDescriptorSetLayout, 4> geomSetLayouts = {
        m_pbrMaterialDSL,
        boneDescriptorSetLayout,
        m_lightUBODescriptorSetLayout,
        m_shadowSamplerDescriptorSetLayout
    };
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset     = 0;
    pcRange.size       = 128;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = static_cast<uint32_t>(geomSetLayouts.size());
    layoutInfo.pSetLayouts            = geomSetLayouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_gbufferGeomLayout) != VK_SUCCESS) {
        SLEAK_ERROR("PBR: Failed to create GBuffer geometry pipeline layout!");
        return false;
    }

    m_pbrMaterialResourcesCreated = true;
    SLEAK_INFO("VulkanRenderer: PBR material resources created");
    return true;
}

/// Allocates a descriptor pool, PBR_SETS_PER_FRAME sets and a mapped UBO for
/// one frame.
bool VulkanRenderer::AddPBRMaterialChunk(uint32_t frame) {
    PBRMaterialChunk chunk{};

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 6 * PBR_SETS_PER_FRAME;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1 * PBR_SETS_PER_FRAME;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = PBR_SETS_PER_FRAME;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &chunk.pool) !=
        VK_SUCCESS) {
        SLEAK_ERROR("PBR: Failed to create PBR material pool!");
        return false;
    }

    std::array<VkDescriptorSetLayout, PBR_SETS_PER_FRAME> layouts;
    layouts.fill(m_pbrMaterialDSL);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = chunk.pool;
    allocInfo.descriptorSetCount = PBR_SETS_PER_FRAME;
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &allocInfo, chunk.sets.data()) !=
        VK_SUCCESS) {
        SLEAK_ERROR("PBR: Failed to allocate PBR material descriptor sets!");
        vkDestroyDescriptorPool(device, chunk.pool, nullptr);
        return false;
    }

    const VkDeviceSize uboSize = m_pbrMaterialUBOStride * PBR_SETS_PER_FRAME;
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = uboSize;
    bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufInfo, nullptr, &chunk.ubo) != VK_SUCCESS) {
        SLEAK_ERROR("PBR: Failed to create material UBO buffer for frame {}!",
                    frame);
        vkDestroyDescriptorPool(device, chunk.pool, nullptr);
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, chunk.ubo, &memReqs);
    VkMemoryAllocateInfo memInfo{};
    memInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memInfo.allocationSize = memReqs.size;
    memInfo.memoryTypeIndex = FindMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &memInfo, nullptr, &chunk.memory) !=
        VK_SUCCESS) {
        SLEAK_ERROR("PBR: Failed to allocate material UBO memory for frame {}!",
                    frame);
        vkDestroyBuffer(device, chunk.ubo, nullptr);
        vkDestroyDescriptorPool(device, chunk.pool, nullptr);
        return false;
    }
    vkBindBufferMemory(device, chunk.ubo, chunk.memory, 0);
    vkMapMemory(device, chunk.memory, 0, uboSize, 0, &chunk.mapped);

    m_pbrMaterialChunks[frame].push_back(chunk);
    if (m_pbrMaterialChunks[frame].size() > 1)
        SLEAK_INFO("PBR: frame {} material ring grew to {} sets", frame,
                   m_pbrMaterialChunks[frame].size() * PBR_SETS_PER_FRAME);
    return true;
}

/// Frees every chunk's UBO, memory and descriptor pool for all frames.
void VulkanRenderer::DestroyPBRMaterialChunks() {
    for (auto& chunks : m_pbrMaterialChunks) {
        for (auto& chunk : chunks) {
            if (chunk.mapped) vkUnmapMemory(device, chunk.memory);
            if (chunk.ubo) vkDestroyBuffer(device, chunk.ubo, nullptr);
            if (chunk.memory) vkFreeMemory(device, chunk.memory, nullptr);
            if (chunk.pool)
                vkDestroyDescriptorPool(device, chunk.pool, nullptr);
        }
        chunks.clear();
    }
}

/// Writes a material's textures and params into its ring slot and binds it at set 0.
void VulkanRenderer::BindPBRMaterial(Sleak::Material* material) {
    if (!bFrameStarted || !m_pbrMaterialResourcesCreated || !material) return;

    // Switch to the default GBuffer pipeline and drop the active custom format
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufferPipeline);
    m_activeCustomFormat = 0;

    // Claim this material's own ring slot (own set + own UBO region) so the
    // set is never rewritten while already bound by a prior draw this frame.
    const uint32_t slot = m_pbrMaterialSlot[currentFrame];
    auto& chunks = m_pbrMaterialChunks[currentFrame];
    const uint32_t chunkIdx = slot / PBR_SETS_PER_FRAME;
    if (chunkIdx >= chunks.size() && !AddPBRMaterialChunk(currentFrame)) return;
    PBRMaterialChunk& chunk = chunks[chunkIdx];
    const uint32_t local = slot % PBR_SETS_PER_FRAME;
    const VkDescriptorSet set = chunk.sets[local];
    const VkDeviceSize uboOffset = local * m_pbrMaterialUBOStride;

    // Build PBRMaterialParams from Material properties
    PBRMaterialParams params{};
    auto dc = material->GetDiffuseColor();
    params.albedoFactorR     = dc.GetR() / 255.0f;
    params.albedoFactorG     = dc.GetG() / 255.0f;
    params.albedoFactorB     = dc.GetB() / 255.0f;
    params.albedoFactorA     = material->GetOpacity();
    params.metallicFactor    = material->GetMetallic();
    params.roughnessFactor   = material->GetRoughness();
    params.aoFactor          = material->GetAO();
    params.normalIntensity   = material->GetNormalIntensity();
    auto ec = material->GetEmissiveColor();
    params.emissiveR         = ec.GetR() / 255.0f;
    params.emissiveG         = ec.GetG() / 255.0f;
    params.emissiveB         = ec.GetB() / 255.0f;
    params.emissiveIntensity = material->GetEmissiveIntensity();
    auto tiling              = material->GetTiling();
    params.tilingX           = tiling.GetX();
    params.tilingY           = tiling.GetY();
    auto offset              = material->GetOffset();
    params.offsetX           = offset.GetX();
    params.offsetY           = offset.GetY();
    params.hasNormalMap      = material->HasNormalTexture()    ? 1u : 0u;
    params.hasMetallicMap    = material->HasMetallicTexture()  ? 1u : 0u;
    params.hasRoughnessMap   = material->HasRoughnessTexture() ? 1u : 0u;
    params.hasAOMap          = material->HasAOTexture()        ? 1u : 0u;
    params.hasEmissiveMap    = material->HasEmissiveTexture()  ? 1u : 0u;

    if (chunk.mapped)
        memcpy(static_cast<char*>(chunk.mapped) + uboOffset, &params,
               sizeof(params));

    // Resolve textures — fall back to the default white 1×1 texture if absent
    auto resolveView = [&](Texture* tex) -> VkImageView {
        auto* vt = tex ? static_cast<VulkanTexture*>(tex) : nullptr;
        if (!vt && m_defaultTexture) vt = m_defaultTexture;
        return vt ? vt->GetImageView() : VK_NULL_HANDLE;
    };
    auto resolveSampler = [&](Texture* tex) -> VkSampler {
        auto* vt = tex ? static_cast<VulkanTexture*>(tex) : nullptr;
        if (!vt && m_defaultTexture) vt = m_defaultTexture;
        return vt ? vt->GetSampler() : VK_NULL_HANDLE;
    };

    Texture* textures[6] = {
        material->GetDiffuseTexture(),
        material->HasNormalTexture()    ? material->GetNormalTexture()    : nullptr,
        material->HasMetallicTexture()  ? material->GetMetallicTexture()  : nullptr,
        material->HasRoughnessTexture() ? material->GetRoughnessTexture() : nullptr,
        material->HasAOTexture()        ? material->GetAOTexture()        : nullptr,
        material->HasEmissiveTexture()  ? material->GetEmissiveTexture()  : nullptr,
    };

    std::array<VkDescriptorImageInfo, 6> imageInfos{};
    for (uint32_t i = 0; i < 6; ++i) {
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[i].imageView   = resolveView(textures[i]);
        imageInfos[i].sampler     = resolveSampler(textures[i]);
    }

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = chunk.ubo;
    bufInfo.offset = uboOffset;
    bufInfo.range  = sizeof(PBRMaterialParams);

    std::array<VkWriteDescriptorSet, 7> writes{};
    for (uint32_t i = 0; i < 6; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding      = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imageInfos[i];
    }
    writes[6].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = set;
    writes[6].dstBinding      = 6;
    writes[6].dstArrayElement = 0;
    writes[6].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[6].descriptorCount = 1;
    writes[6].pBufferInfo     = &bufInfo;

    vkUpdateDescriptorSets(device, 7, writes.data(), 0, nullptr);

    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_gbufferGeomLayout, 0, 1, &set, 0, nullptr);

    // Advance the ring for the next material this frame.
    m_pbrMaterialSlot[currentFrame] = slot + 1;
}

}
}
