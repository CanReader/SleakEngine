#include <array>

#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "Core/Logger.hpp"

namespace Sleak {
namespace RenderEngine {

// Procedural atmosphere: composite pass, sky probe and forward sky

namespace {

constexpr const char* COMPOSITE_FRAGMENT_SPIRV =
    "assets/shaders/atmosphere_composite.frag.spv";
constexpr const char* PROBE_FRAGMENT_SPIRV =
    "assets/shaders/atmosphere_probe.frag.spv";
constexpr const char* FORWARD_SKY_FRAGMENT_SPIRV =
    "assets/shaders/atmosphere_sky.frag.spv";
// SLEAK_PROBE_FACE in atmosphere_probe.frag
constexpr uint32_t PROBE_FACE_CONSTANT_ID = 0;
constexpr uint32_t FULLSCREEN_VERTICES = 3;

}  // namespace

/// True while the deferred path composites, which is when EndRender can
/// draw the sky a skipped skybox leaves open.
bool VulkanRenderer::AtmosphereSupported() const {
    return IsDeferredEnabled() && m_bloomResourcesCreated &&
           !m_atmosphereFailed;
}

/// Builds the atmosphere objects on first use. A failure is logged once and
/// leaves the skybox drawing until the GBuffer is rebuilt.
void VulkanRenderer::EnsureAtmosphereResources() {
    if (m_atmospherePipeline != VK_NULL_HANDLE) return;
    if (CreateAtmosphereTargets() && CreateAtmospherePipelines()) return;
    SLEAK_ERROR("Atmosphere: Vulkan objects failed, the skybox stays");
    CleanupAtmosphereResources();
    m_atmosphereFailed = true;
}

/// Destroys every atmosphere object and forgets a failed creation; the GPU
/// must be idle.
void VulkanRenderer::CleanupAtmosphereResources() {
    if (m_atmospherePipeline)
        vkDestroyPipeline(device, m_atmospherePipeline, nullptr);
    m_atmospherePipeline = VK_NULL_HANDLE;
    if (m_atmosphereFramebuffer)
        vkDestroyFramebuffer(device, m_atmosphereFramebuffer, nullptr);
    m_atmosphereFramebuffer = VK_NULL_HANDLE;
    delete m_atmosphereShader;
    m_atmosphereShader = nullptr;
    m_atmosphereFailed = false;

    delete m_skyProbeShader;
    m_skyProbeShader = nullptr;
    for (uint32_t face = 0; face < CUBE_FACES; ++face) {
        if (m_skyProbePipelines[face])
            vkDestroyPipeline(device, m_skyProbePipelines[face], nullptr);
        if (m_skyProbeFramebuffers[face])
            vkDestroyFramebuffer(device, m_skyProbeFramebuffers[face], nullptr);
        if (m_skyProbeFaceViews[face])
            vkDestroyImageView(device, m_skyProbeFaceViews[face], nullptr);
    }
    m_skyProbePipelines = {};
    m_skyProbeFramebuffers = {};
    m_skyProbeFaceViews = {};
    if (m_skyProbeView) vkDestroyImageView(device, m_skyProbeView, nullptr);
    if (m_skyProbeImage) vkDestroyImage(device, m_skyProbeImage, nullptr);
    if (m_skyProbeMemory) vkFreeMemory(device, m_skyProbeMemory, nullptr);
    m_skyProbeView = VK_NULL_HANDLE;
    m_skyProbeImage = VK_NULL_HANDLE;
    m_skyProbeMemory = VK_NULL_HANDLE;
    m_skyProbeDrawn = false;
    ReleaseCloudNoise();
}

/// Creates what the atmosphere draws into: a framebuffer on the HDR scene
/// for the composite, and the sky probe, a cube in the HDR scene format with
/// a mip chain, a view of all of it and a framebuffer on each face of mip 0.
/// The bloom passes fit both: an HDR target left shader readable.
bool VulkanRenderer::CreateAtmosphereTargets() {
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = m_bloomAddRenderPass;
    fb.attachmentCount = 1;
    fb.pAttachments = &m_hdrSceneView;
    fb.width = scExtent.width;
    fb.height = scExtent.height;
    fb.layers = 1;
    if (vkCreateFramebuffer(device, &fb, nullptr, &m_atmosphereFramebuffer) !=
        VK_SUCCESS)
        return false;

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = m_hdrSceneFormat;
    info.extent = {SKY_PROBE_SIZE, SKY_PROBE_SIZE, 1};
    info.mipLevels = SKY_PROBE_MIPS;
    info.arrayLayers = CUBE_FACES;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &info, nullptr, &m_skyProbeImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, m_skyProbeImage, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &m_skyProbeMemory) !=
        VK_SUCCESS)
        return false;
    vkBindImageMemory(device, m_skyProbeImage, m_skyProbeMemory, 0);

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = m_skyProbeImage;
    view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    view.format = m_hdrSceneFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, SKY_PROBE_MIPS, 0,
                             CUBE_FACES};
    if (vkCreateImageView(device, &view, nullptr, &m_skyProbeView) !=
        VK_SUCCESS)
        return false;

    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    fb.renderPass = m_bloomRenderPass;
    fb.width = SKY_PROBE_SIZE;
    fb.height = SKY_PROBE_SIZE;
    for (uint32_t face = 0; face < CUBE_FACES; ++face) {
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1};
        fb.pAttachments = &m_skyProbeFaceViews[face];
        if (vkCreateImageView(device, &view, nullptr,
                              &m_skyProbeFaceViews[face]) != VK_SUCCESS ||
            vkCreateFramebuffer(device, &fb, nullptr,
                                &m_skyProbeFramebuffers[face]) != VK_SUCCESS)
            return false;
    }
    // A new view can reuse an old handle: scene-read sets must rewrite it
    ++m_skyboxViewSerial;
    return true;
}

/// Compiles the composite and probe shaders and builds their pipelines on
/// the lighting pass layout, one probe pipeline per cube face.
bool VulkanRenderer::CreateAtmospherePipelines() {
    m_atmosphereShader = new VulkanShader(device);
    m_skyProbeShader = new VulkanShader(device);
    if (!m_atmosphereShader->compile(LIGHTING_VERTEX_SPIRV,
                                     COMPOSITE_FRAGMENT_SPIRV) ||
        !m_skyProbeShader->compile(LIGHTING_VERTEX_SPIRV, PROBE_FRAGMENT_SPIRV))
        return false;

    // dst = src.rgb + dst.rgb * src.a; alpha, the TAA reactive mask, stays
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT;
    m_atmospherePipeline = BuildLightingPipeline(*m_atmosphereShader,
                                                 m_bloomAddRenderPass, &blend);
    if (m_atmospherePipeline == VK_NULL_HANDLE) return false;

    for (uint32_t face = 0; face < CUBE_FACES; ++face) {
        const VkSpecializationMapEntry entry = {PROBE_FACE_CONSTANT_ID, 0,
                                                sizeof(face)};
        VkSpecializationInfo constants{};
        constants.mapEntryCount = 1;
        constants.pMapEntries = &entry;
        constants.dataSize = sizeof(face);
        constants.pData = &face;
        m_skyProbePipelines[face] = BuildLightingPipeline(
            *m_skyProbeShader, m_bloomRenderPass, nullptr, &constants);
        if (m_skyProbePipelines[face] == VK_NULL_HANDLE) return false;
    }
    return true;
}

/// Draws sky and fog over the HDR scene at final depth, blended into it in
/// the bloom add pass.
void VulkanRenderer::RenderAtmospherePass() {
    if (m_atmospherePipeline == VK_NULL_HANDLE) return;
    TransitionDepthForPostPasses();

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_bloomAddRenderPass;
    rp.framebuffer = m_atmosphereFramebuffer;
    rp.renderArea.extent = scExtent;

    BeginGpuPass(GpuPass::Atmosphere);
    vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    FillFullscreenViewportScissor(command, scExtent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_atmospherePipeline);
    BindLightingSets();
    vkCmdDraw(command, FULLSCREEN_VERTICES, 1, 0, 0);
    vkCmdEndRenderPass(command);
    EndGpuPass(GpuPass::Atmosphere);
}

/// Binds the lighting pass's four sets on its layout: GBuffer, depth and
/// cloud noise in set 0, the frame blocks in set 2.
void VulkanRenderer::BindLightingSets() {
    const VkDescriptorSet sets[] = {
        m_gbufferSamplerSets[currentFrame], m_deferredCBSets[currentFrame],
        m_lightUBODescriptorSets[currentFrame], m_iblSets[currentFrame]};
    vkCmdBindDescriptorSets(
        command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipelineLayout, 0,
        static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
}

/// Draws the sky gradient and clouds into the probe faces and blits their
/// mips, so this frame's reflections see this frame's sky.
void VulkanRenderer::RenderSkyProbe() {
    if (!DrawsAtmosphere() || m_atmospherePipeline == VK_NULL_HANDLE) return;
    // shortcut: redrawn every frame, add a change test if --gpuprofile
    // shows the probe above 0.05 ms
    BeginGpuPass(GpuPass::Atmosphere);

    const VkExtent2D extent = {SKY_PROBE_SIZE, SKY_PROBE_SIZE};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_bloomRenderPass;
    rp.renderArea.extent = extent;
    BindLightingSets();
    for (uint32_t face = 0; face < CUBE_FACES; ++face) {
        rp.framebuffer = m_skyProbeFramebuffers[face];
        vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
        FillFullscreenViewportScissor(command, extent);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_skyProbePipelines[face]);
        vkCmdDraw(command, FULLSCREEN_VERTICES, 1, 0, 0);
        vkCmdEndRenderPass(command);
    }

    // Mips: every level is blitted from the one above, all faces at once
    const auto barrier = [this](uint32_t firstMip, uint32_t mips,
                                VkImageLayout from, VkImageLayout to,
                                VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.image = m_skyProbeImage;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, firstMip, mips, 0,
                              CUBE_FACES};
        return b;
    };
    const VkImageMemoryBarrier toBlit[] = {
        barrier(0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT),
        barrier(1, SKY_PROBE_MIPS - 1, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT)};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, static_cast<uint32_t>(std::size(toBlit)),
                         toBlit);
    for (uint32_t mip = 1; mip < SKY_PROBE_MIPS; ++mip) {
        const auto srcSize = static_cast<int32_t>(SKY_PROBE_SIZE >> (mip - 1));
        const auto dstSize = static_cast<int32_t>(SKY_PROBE_SIZE >> mip);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0,
                               CUBE_FACES};
        blit.srcOffsets[1] = {srcSize, srcSize, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, CUBE_FACES};
        blit.dstOffsets[1] = {dstSize, dstSize, 1};
        vkCmdBlitImage(command, m_skyProbeImage,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_skyProbeImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        const VkImageMemoryBarrier toSource =
            barrier(mip, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toSource);
    }
    const VkImageMemoryBarrier toRead =
        barrier(0, SKY_PROBE_MIPS, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toRead);

    m_skyProbeDrawn = true;
    EndGpuPass(GpuPass::Atmosphere);
}

/// Cube and sampler for the scene-read sky slot: the probe once the
/// atmosphere has drawn it, else the active scene's skybox, else the black
/// stand-in.
VkDescriptorImageInfo VulkanRenderer::SceneSky() const {
    constexpr VkImageLayout LAYOUT = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (DrawsAtmosphere()) {
        if (m_skyProbeDrawn)
            return {m_iblPrefilterSampler, m_skyProbeView, LAYOUT};
    } else if (const VulkanCubemapTexture* skybox = SceneSkyboxCubemap()) {
        return {skybox->GetSampler(), skybox->GetImageView(), LAYOUT};
    }
    return {m_sceneLinearSampler, m_sceneFallbackCubeView, LAYOUT};
}

/// True on the forward path until the forward sky fails to build.
bool VulkanRenderer::ForwardSkySupported() const {
    return !IsDeferredEnabled() && !m_forwardSkyFailed;
}

/// Draws the atmosphere's sky as the forward pass's first draw: the scene
/// covers it, so it needs no depth test. The scene's commands then start
/// from BeginRender's pipeline and texture set.
void VulkanRenderer::RenderForwardSky() {
    if (!bFrameStarted || !m_lightUBOCreated || !DrawsForwardSky()) return;
    if (m_forwardSkyPipeline == VK_NULL_HANDLE && !CreateForwardSkyPipeline())
        return;
    // Timed in the atmosphere row, out of the forward pass
    EndGpuPass(GpuPass::Forward);
    BeginGpuPass(GpuPass::Atmosphere);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_forwardSkyPipeline);
    // The cloud noise is the sky's set 0 texture (atmosphere_sky.frag)
    if (m_cloudNoise) BindTextureRaw(m_cloudNoise.get(), 0);
    vkCmdDraw(command, FULLSCREEN_VERTICES, 1, 0, 0);
    EndGpuPass(GpuPass::Atmosphere);
    BeginGpuPass(GpuPass::Forward);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (m_cloudNoise && m_textureDescriptorsWritten &&
        CurrentFrameIndex < descriptorSets.size())
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLay, 0, 1,
                                &descriptorSets[CurrentFrameIndex], 0, nullptr);
}

/// Compiles the forward sky shaders and builds its pipeline for the main
/// pass on the forward layout, at the pass's sample count.
bool VulkanRenderer::CreateForwardSkyPipeline() {
    if (!m_forwardSkyShader) {
        m_forwardSkyShader = new VulkanShader(device);
        if (!m_forwardSkyShader->compile(LIGHTING_VERTEX_SPIRV,
                                         FORWARD_SKY_FRAGMENT_SPIRV)) {
            delete m_forwardSkyShader;
            m_forwardSkyShader = nullptr;
        }
    }
    if (m_forwardSkyShader)
        m_forwardSkyPipeline =
            BuildLightingPipeline(*m_forwardSkyShader, renderPass, nullptr,
                                  nullptr, pipelineLay, m_msaaSamples);
    if (m_forwardSkyPipeline != VK_NULL_HANDLE) return true;
    SLEAK_ERROR("Atmosphere: Vulkan forward sky failed, the skybox stays");
    m_forwardSkyFailed = true;
    return false;
}

}  // namespace RenderEngine
}  // namespace Sleak
