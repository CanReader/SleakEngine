#include <array>

#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "Core/Logger.hpp"

namespace Sleak {
namespace RenderEngine {

// Procedural atmosphere: composite pass

namespace {

constexpr const char* COMPOSITE_FRAGMENT_SPIRV =
    "assets/shaders/atmosphere_composite.frag.spv";
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
}

/// Creates the framebuffer the composite draws into: the HDR scene, in the
/// bloom add pass, which loads it and leaves it shader readable.
bool VulkanRenderer::CreateAtmosphereTargets() {
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = m_bloomAddRenderPass;
    fb.attachmentCount = 1;
    fb.pAttachments = &m_hdrSceneView;
    fb.width = scExtent.width;
    fb.height = scExtent.height;
    fb.layers = 1;
    return vkCreateFramebuffer(device, &fb, nullptr,
                               &m_atmosphereFramebuffer) == VK_SUCCESS;
}

/// Compiles the composite shader and builds its pipeline on the lighting
/// pass layout.
bool VulkanRenderer::CreateAtmospherePipelines() {
    m_atmosphereShader = new VulkanShader(device);
    if (!m_atmosphereShader->compile(LIGHTING_VERTEX_SPIRV,
                                     COMPOSITE_FRAGMENT_SPIRV))
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
    return m_atmospherePipeline != VK_NULL_HANDLE;
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
    // The lighting pass sets: depth in set 0, the frame blocks in set 2
    const VkDescriptorSet sets[] = {
        m_gbufferSamplerSets[currentFrame], m_deferredCBSets[currentFrame],
        m_lightUBODescriptorSets[currentFrame], m_iblSets[currentFrame]};
    vkCmdBindDescriptorSets(
        command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipelineLayout, 0,
        static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
    vkCmdDraw(command, FULLSCREEN_VERTICES, 1, 0, 0);
    vkCmdEndRenderPass(command);
    EndGpuPass(GpuPass::Atmosphere);
}

}  // namespace RenderEngine
}  // namespace Sleak
