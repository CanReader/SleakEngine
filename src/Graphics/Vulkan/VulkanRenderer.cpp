#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanBuffer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanTexture.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanCubemapTexture.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanInternal.hpp"
#include "../../include/private/Graphics/Common/RenderCommandQueue.hpp"
#include <Runtime/MeshData.hpp>

#include <SDL3/SDL_vulkan.h>
#include <Core/Window.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "Graphics/Vulkan/VulkanShader.hpp"
#include "Graphics/Common/ResourceManager.hpp"
#include "Core/Logger.hpp"
#include "Core/CommandLine.hpp"
#include "Camera/Camera.hpp"
#include "Math/Matrix.hpp"
#include <random>
#include "SDL3/SDL_error.h"
#include "SDL3/SDL_video.h"
#ifdef PLATFORM_LINUX
    #include "vulkan/vulkan_wayland.h"
#elif defined(PLATFORM_WIN)
    #include <vulkan/vulkan_win32.h>
#endif

namespace Sleak {
    namespace RenderEngine {

VulkanRenderer::VulkanRenderer(Window* window)
    : sdlWindow(window) {
    this->Type = RendererType::Vulkan;
    clearColor = {{0.3f, 0.4f, 1.0f, 1.0f}};

    ResourceManager::RegisterCreateBuffer(
        this, &VulkanRenderer::CreateBuffer);
    ResourceManager::RegisterCreateShader(
        this, &VulkanRenderer::CreateShader);
    ResourceManager::RegisterCreateTexture(
        this, &VulkanRenderer::CreateTexture);
    ResourceManager::RegisterCreateCubemapTexture(
        this, &VulkanRenderer::CreateCubemapTexture);
    ResourceManager::RegisterCreateCubemapTextureFromPanorama(
        this, &VulkanRenderer::CreateCubemapTextureFromPanorama);

    ResourceManager::RegisterCreateTextureFromMemory(
        [this](const void* data, uint32_t w, uint32_t h, TextureFormat fmt, uint32_t maxMip) -> ::Sleak::Texture* {
            auto* tex = new VulkanTexture(device, physicalDevice, commands, graphicsQueue);
            tex->SetMaxMipLevels(maxMip);
            if (tex->LoadFromMemory(data, w, h, fmt)) {
                WriteTextureDescriptors(tex);
                return tex;
            }
            delete tex;
            return nullptr;
        });
}

VulkanRenderer::~VulkanRenderer() {
    Cleanup();
}

bool VulkanRenderer::Initialize() {
    if (!InitVulkan())
        SLEAK_RETURN_ERR("Failed to initialize Vulkan Instance!");

    if (!SetupDebugMessenger()) {
        SLEAK_WARN("Failed to setup validation layer of vulkan instance")
    }

    if (!CreateSurface())
        SLEAK_RETURN_ERR("Failed to create render surface!")

    if (!CreateDevice())
        SLEAK_RETURN_ERR("Failed to initialize devices!");

    if (!CreateSwapChain())
        SLEAK_RETURN_ERR("Failed to create swap chain!");

    if (!CreateImageViews())
        SLEAK_RETURN_ERR("Failed to create image views for renderer!");

    if (!CreateDepthResources())
        SLEAK_RETURN_ERR("Failed to create depth resources!");

    if (!CreateMSAAColorResources())
        SLEAK_RETURN_ERR("Failed to create MSAA color resources!");

    if (!CreateRenderPass())
        SLEAK_RETURN_ERR("Failed to create a render pass for the renderer!");

    if (!CreateDescriptorSetLayout())
        SLEAK_RETURN_ERR("Failed to create descriptor set layout!");

    if (!CreateDescriptorPool())
        SLEAK_RETURN_ERR("Failed to create descriptor pool!");

    if (!AllocateDescriptorSets())
        SLEAK_RETURN_ERR("Failed to allocate descriptor sets!");

    if (!CreateCommandPool())
        SLEAK_RETURN_ERR("Failed to create command pool for renderer!");

    if (!CreateCommandBuffer())
        SLEAK_RETURN_ERR("Failed to create command buffers for renderer!");

    if (!CreateDefaultTexture())
        SLEAK_WARN("Failed to create default white texture for Vulkan");

    if (!CreateGraphicsPipeline())
        SLEAK_RETURN_ERR("Failed to create graphics pipeline!");

    if (!CreateShadowLightUBOResources())
        SLEAK_WARN("Failed to create light UBO resources — dynamic lighting disabled");

    if (!CreateShadowResources())
        SLEAK_WARN("Failed to create shadow mapping resources — shadows disabled");

    if (!CreateFrameBuffer())
        SLEAK_RETURN_ERR("Failed to create framebuffer of renderer!");

    // Deferred GBuffer — initialized after swapchain framebuffers are ready
    if (m_deferredEnabled) {
        if (!CreateGBufferResources())
            SLEAK_WARN("Failed to create GBuffer resources — deferred rendering disabled");
    }

    // Eagerly create bone UBO resources so set 1 is always bound at pass start.
    // Must happen after CreateDescriptorSetLayout() (boneDescriptorSetLayout is ready)
    // and after GBuffer init (m_deferredEnabled is known).
    if (!CreateBoneUBOResources())
        SLEAK_WARN("Failed to pre-create bone UBO resources — skinned meshes may malfunction on first frame");

    if (!CreateSyncObjects())
        SLEAK_RETURN_ERR("Failed to synchronization objects of renderer!");

    SetPerformanceCounter(true);

    SLEAK_INFO("Vulkan renderer has been initialized successfully!");

    return true;
}

// BeginRender: prepare the command buffer, begin render pass.
// Do NOT end the command buffer here — the RenderCommandQueue will
// record draw commands via the RenderContext interface.
void VulkanRenderer::BeginRender() {
    bFrameStarted = false;
    m_inGeometryPass = false;
    m_inForwardTransparentPass = false;
    m_forwardPassOpen = false;
    m_inVoxelPass = false;
    if (!bRender)
        return;

    // Commit staged lightVP. Do this BEFORE the shadow pass so the shadow
    // map and the main pass both read the same m_lightVP this frame.
    if (m_hasPendingLightVP) {
        memcpy(m_lightVP, m_pendingLightVP, sizeof(m_lightVP));
    }

    // Apply pending changes between frames
    if (m_vsyncChangeRequested)
        ApplyVSyncChange();
    if (m_msaaChangeRequested)
        ApplyMSAAChange();
    if (m_shadowResChangeRequested)
        ApplyShadowResolutionChange();

    VkResult result;

    if (device && !inFlightFences.empty()) {
        vkWaitForFences(device, 1, &inFlightFences[currentFrame],
                        VK_TRUE, UINT64_MAX);
    }

    // Clean up staging buffers from the previous use of this frame slot.
    // The fence wait above guarantees the GPU finished both the transfer
    // (waited on by the render submit) and the render itself.
    auto& flush = m_asyncFlush[currentFrame];
    if (flush.submitted) {
        // Free command buffer FIRST to release references to staging buffers
        if (flush.commandBuffer != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(flush.device, flush.commandPool, 1,
                                 &flush.commandBuffer);
        }
        for (auto& pending : flush.stagingBuffers) {
            vmaDestroyBuffer(VulkanBuffer::GetAllocator(), pending.buffer,
                             pending.memory);
            VulkanBuffer::UntrackAllocation(pending.allocSize,
                                            pending.memoryTypeIndex);
        }
        flush = {};
    }

    VulkanBuffer::ProcessDeferredDeletions(MAX_FRAMES_IN_FLIGHT);
    VulkanBuffer::AdvanceDeletionFrame();

    // Enable batched buffer uploads for this frame (async, zero CPU blocking).
    // This is disabled during init/scene transitions where buffers may be
    // created and destroyed before a flush.
    VulkanBuffer::SetBatchingEnabled(true);

    // Acquire the next image from the swapchain
    result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX,
                                    imageAvailableSemaphores[m_semaphoreIndex],
                                    VK_NULL_HANDLE, &CurrentFrameIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapChain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        SLEAK_ERROR("Failed to acquire swapchain image!");
        return;
    }

    // Wait if this swapchain image is still in use by a DIFFERENT frame slot
    if (CurrentFrameIndex < imagesInFlight.size() &&
        imagesInFlight[CurrentFrameIndex] != VK_NULL_HANDLE &&
        imagesInFlight[CurrentFrameIndex] != inFlightFences[currentFrame]) {
        vkWaitForFences(device, 1, &imagesInFlight[CurrentFrameIndex],
                        VK_TRUE, UINT64_MAX);
    }
    imagesInFlight[CurrentFrameIndex] = inFlightFences[currentFrame];

    // Reset the fence only after all waits are done
    vkResetFences(device, 1, &inFlightFences[currentFrame]);

    // Select the command buffer for this frame-in-flight
    command = commandBuffers[currentFrame];

    // Reset and begin the command buffer
    vkResetCommandBuffer(command, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = 0;

    if (vkBeginCommandBuffer(command, &beginInfo) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to begin command buffer!");
        return;
    }

    bFrameStarted = true;
    m_pbrMaterialSlot[currentFrame] = 0;  // reset PBR material ring for this frame

    // Skip shadow pass if no cached draws — preserve previous frame's shadow map
    auto* shadowQueue = RenderCommandQueue::GetInstance();
    bool hasShadowDraws = shadowQueue && shadowQueue->HasCachedShadowDraws();

    if (m_shadowResourcesCreated && m_shadowPassEnabled && hasShadowDraws) {
        VkClearValue shadowClear{};
        shadowClear.depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo shadowPassInfo{};
        shadowPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        shadowPassInfo.renderPass = m_shadowRenderPass;
        shadowPassInfo.framebuffer = m_shadowFramebuffer;
        shadowPassInfo.renderArea.offset = {0, 0};
        shadowPassInfo.renderArea.extent = {m_shadowMapResolution, m_shadowMapResolution};
        shadowPassInfo.clearValueCount = 1;
        shadowPassInfo.pClearValues = &shadowClear;

        vkCmdBeginRenderPass(command, &shadowPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

        // shadow_depth.vert statically declares `layout(set = 1, binding = 0) uniform BoneUBO`
        // (skinning conditioned on boneWeights). The shader must have set 1 bound even for
        // non-skinned casters, otherwise vkCmdDrawIndexed fires VUID-vkCmdDrawIndexed-None-08600.
        // Bind the bone UBO once at pass start so all shadow draws (skinned or static) are legal.
        if (m_boneUBOCreated) {
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLay, 1, 1,
                                    &boneDescriptorSets[currentFrame], 0, nullptr);
        }

        VkViewport shadowViewport{};
        shadowViewport.x = 0.0f;
        shadowViewport.y = 0.0f;
        shadowViewport.width = static_cast<float>(m_shadowMapResolution);
        shadowViewport.height = static_cast<float>(m_shadowMapResolution);
        shadowViewport.minDepth = 0.0f;
        shadowViewport.maxDepth = 1.0f;
        vkCmdSetViewport(command, 0, 1, &shadowViewport);

        VkRect2D shadowScissor{};
        shadowScissor.offset = {0, 0};
        shadowScissor.extent = {m_shadowMapResolution, m_shadowMapResolution};
        vkCmdSetScissor(command, 0, 1, &shadowScissor);

        m_shadowPassActive = true;
        m_shadowPCCacheValid = false;
        auto* queue = RenderCommandQueue::GetInstance();
        if (queue) {
            queue->ExecuteShadowPass(this);
        }
        m_shadowPassActive = false;

        vkCmdEndRenderPass(command);
    }

    // ---- Deferred path: begin GBuffer render pass ----
    if (m_gbufferResourcesCreated && m_deferredEnabled) {
        // 4 clear values: RT0, RT1, RT2, depth
        VkClearValue gbufferClears[GBUFFER_COUNT + 1];
        for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
            gbufferClears[i].color = {0.0f, 0.0f, 0.0f, 0.0f};
        }
        gbufferClears[GBUFFER_COUNT].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo gbufferPassInfo{};
        gbufferPassInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        gbufferPassInfo.renderPass        = m_gbufferRenderPass;
        gbufferPassInfo.framebuffer       = m_gbufferFramebuffer;
        gbufferPassInfo.renderArea.offset = {0, 0};
        gbufferPassInfo.renderArea.extent = scExtent;
        gbufferPassInfo.clearValueCount   = GBUFFER_COUNT + 1;
        gbufferPassInfo.pClearValues      = gbufferClears;

        vkCmdBeginRenderPass(command, &gbufferPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufferPipeline);

        // Set viewport and scissor
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

        // Bind descriptor sets for GBuffer geometry pass.
        // Set 0 (PBR material) is bound per-material by BindPBRMaterial().
        // Set 1 (bone matrices) is frame-constant: bound here so m_skinnedGbufferPipeline
        //   can always find a valid set 1, even for frames where no skinned draw fires.
        // Sets 2-3 are frame-constant: light/shadow UBO and shadow samplers.
        if (m_gbufferGeomLayout != VK_NULL_HANDLE && m_lightUBOCreated) {
            if (m_boneUBOCreated) {
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_gbufferGeomLayout, 1, 1,
                                        &boneDescriptorSets[currentFrame], 0, nullptr);
            }
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_gbufferGeomLayout, 2, 1,
                                    &m_lightUBODescriptorSets[currentFrame], 0, nullptr);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_gbufferGeomLayout, 3, 1,
                                    &m_shadowSamplerDescriptorSets[currentFrame], 0, nullptr);
        }

        m_inGeometryPass = true;

        // ImGui new frame (same as forward path below)
        bImFrameActive = false;
        if (bImInitialized) {
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            auto& io = ImGui::GetIO();
            if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
                ImGui::NewFrame();
                bImFrameActive = true;
            }
        }
        return;
    }

    // ---- Forward path (non-deferred): begin main render pass ----
    // When MSAA: 3 attachments (color, depth, resolve); otherwise 2
    // Use stack array to avoid per-frame heap allocation
    VkClearValue clearValues[3];
    uint32_t clearValueCount = 2;
    clearValues[0] = clearColor;
    clearValues[1].depthStencil = {1.0f, 0};
    if (m_msaaSamples != VK_SAMPLE_COUNT_1_BIT) {
        clearValues[2].color = clearColor.color;
        clearValueCount = 3;
    }

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = renderPass;
    passInfo.framebuffer = swapChainFramebuffers[CurrentFrameIndex];
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = scExtent;
    passInfo.clearValueCount = clearValueCount;
    passInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(command, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Bind texture descriptor set if available
    if (m_textureDescriptorsWritten &&
        CurrentFrameIndex < descriptorSets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &descriptorSets[CurrentFrameIndex], 0, nullptr);
    }

    // Set dynamic viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(scExtent.width);
    viewport.height = static_cast<float>(scExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = scExtent;
    vkCmdSetScissor(command, 0, 1, &scissor);

    // Bind light UBO at set 2 and shadow sampler at set 3
    if (m_lightUBOCreated) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 2, 1,
            &m_lightUBODescriptorSets[currentFrame], 0, nullptr);
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 3, 1,
            &m_shadowSamplerDescriptorSets[currentFrame], 0, nullptr);
    }

    // RenderCommandQueue will now call Draw/DrawIndexed/Bind* methods
    // via the RenderContext interface on this object

    bImFrameActive = false;
    if (bImInitialized) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        auto& io = ImGui::GetIO();
        if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
            ImGui::NewFrame();
            bImFrameActive = true;
        }
    }
}

// EndRender: end render pass, end command buffer, submit, present.
void VulkanRenderer::EndRender() {
    if (!bRender || !bFrameStarted)
        return;

    const bool deferredPath =
        m_gbufferResourcesCreated && m_deferredEnabled && m_bloomResourcesCreated;

    // Safety: if geometry pass is still open (ExecuteDeferredLightingPass not called),
    // end it now so we don't have a dangling render pass.
    if (m_gbufferResourcesCreated && m_deferredEnabled && m_inGeometryPass) {
        vkCmdEndRenderPass(command);
        m_inGeometryPass = false;
    }

    // In deferred mode, open a forward render pass so the HDR scene image ends
    // in SHADER_READ_ONLY_OPTIMAL regardless of whether any transparent pass
    // ran. BeginForwardTransparentPass already opens this RP; if the game
    // didn't call it (no transparent objects), open/close a trivial one here.
    if (deferredPath && !m_forwardPassOpen && !m_inGeometryPass) {
        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass        = m_forwardRenderPass;
        rpBegin.framebuffer       = m_forwardFramebuffers[CurrentFrameIndex];
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = scExtent;
        rpBegin.clearValueCount   = 0;
        vkCmdBeginRenderPass(command, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        m_forwardPassOpen = true;
    }

    // Close the forward pass (HDR scene → SHADER_READ_ONLY_OPTIMAL via finalLayout).
    if (m_forwardPassOpen) {
        vkCmdEndRenderPass(command);
        m_forwardPassOpen = false;
    }

    if (deferredPath) {
        // TAA: accumulate current HDR frame with history, write resolved result
        // back into hdrScene. Also handles the depth barrier (ATTACHMENT → READ_ONLY)
        // so SSR can skip its own barrier when TAA is enabled.
        RenderTAAPass();
        // Screen-space reflections — reads TAA-resolved hdrScene + GBuffer.
        RenderSSRPass();
        // Bloom pyramid generates the bloom result from the HDR scene.
        RenderBloomPass();
        // Composite pass reads HDR scene + bloom + SSR, applies ACES + gamma,
        // writes to the swapchain. ImGui is drawn inside this pass.
        RenderBloomCompositePass();
    } else {
        // Forward (non-deferred) path — ImGui inside main render pass.
        if (bImFrameActive) {
            ImGui::Render();
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
        }
        vkCmdEndRenderPass(command);
    }

    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to end command buffer!");
        return;
    }

    // Submit
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    // Wait on image-available; also wait on transfer semaphore if uploads happened
    VkSemaphore waitSemaphores[2];
    VkPipelineStageFlags waitStages[2];
    uint32_t waitCount = 0;

    waitSemaphores[waitCount] = imageAvailableSemaphores[m_semaphoreIndex];
    waitStages[waitCount] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    waitCount++;

    if (m_asyncFlush[currentFrame].submitted) {
        waitSemaphores[waitCount] = m_transferSemaphores[currentFrame];
        waitStages[waitCount] = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        waitCount++;
    }

    submitInfo.waitSemaphoreCount = waitCount;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command;

    // Index renderFinished semaphore by acquired image index: when image N
    // is re-acquired, the previous present of image N is guaranteed complete,
    // so renderFinishedSemaphores[N] is safe to reuse.
    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[CurrentFrameIndex]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo,
                       inFlightFences[currentFrame]) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to submit draw command buffer!");
    }

    // Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = {swapChain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &CurrentFrameIndex;

    VkResult presentResult = vkQueuePresentKHR(presentQueue, &presentInfo);

    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
        presentResult == VK_SUBOPTIMAL_KHR) {
        RecreateSwapChain();
    } else if (presentResult != VK_SUCCESS) {
        SLEAK_ERROR("Failed to present render!");
    }

    // Reset per-frame deferred state flags
    m_forwardPassOpen = false;
    m_inForwardTransparentPass = false;
    m_inGeometryPass = false;
    bFrameStarted = false;   // command buffer submitted; recording is complete

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    m_semaphoreIndex = (m_semaphoreIndex + 1) %
        static_cast<uint32_t>(imageAvailableSemaphores.size());

    UpdateFrameMetrics();
}

void VulkanRenderer::Draw(uint32_t vertexCount) {
    if (!bFrameStarted) return;
    vkCmdDraw(command, vertexCount, 1, 0, 0);
    if (!m_shadowPassActive) {
        DrawnVertices += vertexCount;
        DrawnTriangles += vertexCount / 3;
    }
}

void VulkanRenderer::DrawIndexed(uint32_t indexCount) {
    if (!bFrameStarted) return;
    vkCmdDrawIndexed(command, indexCount, 1, 0, 0, 0);
    if (!m_shadowPassActive) {
        DrawnVertices += indexCount;
        DrawnTriangles += indexCount / 3;
    }
}

void VulkanRenderer::DrawInstance(uint32_t instanceCount,
                                   uint32_t vertexPerInstance) {
    if (!bFrameStarted) return;
    vkCmdDraw(command, vertexPerInstance, instanceCount, 0, 0);
}

void VulkanRenderer::DrawIndexedInstance(uint32_t instanceCount,
                                          uint32_t indexPerInstance) {
    if (!bFrameStarted) return;
    vkCmdDrawIndexed(command, indexPerInstance, instanceCount, 0, 0, 0);
}

void VulkanRenderer::SetRenderFace(RenderFace face) {
    // Vulkan pipeline state is baked — would need pipeline recreation.
    // Store for next pipeline rebuild.
    Face = face;
}

void VulkanRenderer::SetRenderMode(RenderMode mode) {
    // Vulkan pipeline state is baked — would need pipeline recreation.
    Mode = mode;
}

void VulkanRenderer::SetViewport(float x, float y, float width,
                                  float height, float minDepth,
                                  float maxDepth) {
    if (!bFrameStarted) return;
    VkViewport viewport{};
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = minDepth;
    viewport.maxDepth = maxDepth;
    vkCmdSetViewport(command, 0, 1, &viewport);
}

void VulkanRenderer::ClearRenderTarget(float r, float g, float b,
                                        float a) {
    clearColor = {{r, g, b, a}};
}

void VulkanRenderer::ClearDepthStencil(bool clearDepth, bool clearStencil,
                                        float depth, uint8_t stencil) {
    // Handled by render pass clear values
}

void VulkanRenderer::BindVertexBuffer(RefPtr<BufferBase> buffer,
                                       uint32_t slot) {
    if (!bFrameStarted) return;
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer.get());
    if (!vkBuf) return;

    // Switch to/from voxel pipeline based on vertex buffer format
    bool wantVoxel = buffer->IsVoxelFormat();
    if (wantVoxel != m_inVoxelPass) {
        if (wantVoxel) {
            BeginVoxelPass();
        } else {
            EndVoxelPass();
        }
    }

    VkBuffer buffers[] = {vkBuf->GetVkBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(command, slot, 1, buffers, offsets);
}

void VulkanRenderer::BindIndexBuffer(RefPtr<BufferBase> buffer,
                                      uint32_t slot) {
    if (!bFrameStarted) return;
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer.get());
    if (!vkBuf) return;
    vkCmdBindIndexBuffer(command, vkBuf->GetVkBuffer(), 0,
                         VK_INDEX_TYPE_UINT32);
}

void VulkanRenderer::BindConstantBuffer(RefPtr<BufferBase> buffer,
                                         uint32_t slot) {
    if (!bFrameStarted) return;
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer.get());
    if (!vkBuf) return;

    // Use push constants — recorded into the command buffer per draw call
    void* data = vkBuf->GetData();
    if (!data) return;

    uint32_t size = static_cast<uint32_t>(vkBuf->GetSize());
    if (size > 128) size = 128;  // Vulkan guarantees at least 128 bytes

    // Choose the pipeline layout that owns the currently bound pipeline.
    // GBuffer geometry pass uses m_gbufferGeomLayout; all other passes use pipelineLay.
    VkPipelineLayout activeLayout = (m_inGeometryPass && m_gbufferGeomLayout != VK_NULL_HANDLE)
                                    ? m_gbufferGeomLayout : pipelineLay;

    // In the geometry pass (not shadow), apply TAA sub-pixel jitter to WVP.
    // Sub-pixel jitter: add jx*col3 to col0 and jy*col3 to col1.
    // Y is negated because the geometry shader flips gl_Position.y.
    if (m_inGeometryPass && !m_shadowPassActive && m_taaEnabled &&
        (m_taaJitter[0] != 0.0f || m_taaJitter[1] != 0.0f) && size >= 64) {
        float jdata[32];
        memcpy(jdata, data, size);
        const float jx =  m_taaJitter[0] * 2.0f;   // UV → NDC
        const float jy = -m_taaJitter[1] * 2.0f;   // negate for Y-flip
        // GLSL computes WVP_cpu^T * v, so clip.x is dot(col0, v).
        // Adding jx*col3 to col0 adds jx*clip.w to clip.x → uniform NDC shift.
        for (int r = 0; r < 4; ++r) {
            jdata[r * 4 + 0] += jx * jdata[r * 4 + 3];  // col0 += jx * col3
            jdata[r * 4 + 1] += jy * jdata[r * 4 + 3];  // col1 += jy * col3
        }
        vkCmdPushConstants(command, activeLayout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, size, jdata);
        return;
    }

    if (m_shadowPassActive && slot == 0 && size >= 128) {
        // Shadow mode: push [LightVP*World (64)][World (64)].
        // Buffer layout: [WVP (64 bytes)][World (64 bytes)].
        // LightVP is frame-constant, so memoize on World and reuse the result
        // across the many chunk draws that share the identity transform.
        const float* srcWorld = reinterpret_cast<const float*>(
            static_cast<const char*>(data) + 64);

        if (!m_shadowPCCacheValid ||
            memcmp(srcWorld, m_shadowWorldCache, 64) != 0) {
            // shadowWVP = World * LightVP (row-major)
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        sum += srcWorld[r * 4 + k] * m_lightVP[k * 4 + c];
                    }
                    m_shadowPCCache[r * 4 + c] = sum;
                }
            }
            memcpy(&m_shadowPCCache[16], srcWorld, 64);
            memcpy(m_shadowWorldCache, srcWorld, 64);
            m_shadowPCCacheValid = true;
        }

        vkCmdPushConstants(command, activeLayout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, 128, m_shadowPCCache);
    } else {
        vkCmdPushConstants(command, activeLayout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, size, data);
    }
}

BufferBase* VulkanRenderer::CreateBuffer(BufferType type, uint32_t size,
                                          void* data) {
    auto* buffer = new VulkanBuffer(device, physicalDevice, size, type,
                                     commands, graphicsQueue);
    if (!buffer->Initialize(data)) {
        delete buffer;
        return nullptr;
    }
    return buffer;
}

Shader* VulkanRenderer::CreateShader(const std::string& shaderSource) {
    auto* shader = new VulkanShader(device);
    if (shader->compile(shaderSource)) {
        return shader;
    }
    delete shader;
    return nullptr;
}

::Sleak::Texture* VulkanRenderer::CreateTexture(const std::string& TexturePath) {
    auto* texture = new VulkanTexture(device, physicalDevice, commands,
                                       graphicsQueue);
    if (texture->LoadFromFile(TexturePath)) {
        WriteTextureDescriptors(texture);
        return texture;
    }
    delete texture;
    return nullptr;
}

::Sleak::Texture* VulkanRenderer::CreateTextureFromData(uint32_t width,
                                                uint32_t height,
                                                void* data) {
    auto* texture = new VulkanTexture(device, physicalDevice, commands,
                                       graphicsQueue);
    if (texture->LoadFromMemory(data, width, height, TextureFormat::RGBA8)) {
        return texture;
    }
    delete texture;
    return nullptr;
}

::Sleak::Texture* VulkanRenderer::CreateCubemapTexture(
    const std::array<std::string, 6>& facePaths) {
    auto* texture = new VulkanCubemapTexture(device, physicalDevice,
                                              commands, graphicsQueue);
    if (texture->LoadCubemap(facePaths)) {
        // Create skybox pipeline if not already created
        if (skyboxPipeline == VK_NULL_HANDLE) {
            if (!CreateSkyboxPipeline()) {
                SLEAK_ERROR("VulkanRenderer: Failed to create skybox pipeline");
                delete texture;
                return nullptr;
            }
        }

        // Write cubemap to skybox descriptor sets
        m_skyboxCubemapView = texture->GetImageView();
        m_skyboxCubemapSampler = texture->GetSampler();
        for (size_t i = 0; i < skyboxDescriptorSets.size(); i++) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = m_skyboxCubemapView;
            imageInfo.sampler = m_skyboxCubemapSampler;

            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = skyboxDescriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0,
                                   nullptr);
        }
        m_skyboxDescriptorsWritten = true;

        return texture;
    }
    delete texture;
    return nullptr;
}

::Sleak::Texture* VulkanRenderer::CreateCubemapTextureFromPanorama(
    const std::string& panoramaPath) {
    auto* texture = new VulkanCubemapTexture(device, physicalDevice,
                                              commands, graphicsQueue);
    if (texture->LoadEquirectangular(panoramaPath)) {
        // Create skybox pipeline if not already created
        if (skyboxPipeline == VK_NULL_HANDLE) {
            if (!CreateSkyboxPipeline()) {
                SLEAK_ERROR("VulkanRenderer: Failed to create skybox pipeline");
                delete texture;
                return nullptr;
            }
        }

        // Write cubemap to skybox descriptor sets
        m_skyboxCubemapView = texture->GetImageView();
        m_skyboxCubemapSampler = texture->GetSampler();
        for (size_t i = 0; i < skyboxDescriptorSets.size(); i++) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = m_skyboxCubemapView;
            imageInfo.sampler = m_skyboxCubemapSampler;

            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = skyboxDescriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0,
                                   nullptr);
        }
        m_skyboxDescriptorsWritten = true;

        // Trigger IBL precompute now that we have an environment cubemap
        return texture;
    }
    delete texture;
    return nullptr;
}

void VulkanRenderer::BindTexture(RefPtr<Sleak::Texture> texture,
                                  uint32_t slot) {
    if (!bFrameStarted) return;
    if (!texture.IsValid() || slot != 0)
        return;

    // Cubemap textures are bound via skybox pass, skip here
    if (texture->GetType() == TextureType::TextureCube)
        return;

    auto* vkTex = static_cast<VulkanTexture*>(texture.get());
    if (!vkTex || !vkTex->HasDescriptorSets())
        return;

    const auto& sets = vkTex->GetDescriptorSets();
    if (CurrentFrameIndex < sets.size()) {
        // In the GBuffer geometry pass set 0 is the PBR material descriptor set
        // (m_pbrMaterialDSL, bound by BindPBRMaterial via m_gbufferGeomLayout).
        // Binding a forward single-sampler descriptor set here with the wrong
        // layout would corrupt set 0 and trigger VK_ERROR_DEVICE_LOST.
        // BindPBRMaterial owns set 0 during the geometry pass — skip here.
        if (m_inGeometryPass)
            return;
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &sets[CurrentFrameIndex], 0, nullptr);
    }
}

void VulkanRenderer::BindTextureRaw(Sleak::Texture* texture, uint32_t slot) {
    if (!bFrameStarted) return;
    if (!texture || slot != 0)
        return;

    // Cubemap textures are bound via skybox pass, skip here
    if (texture->GetType() == TextureType::TextureCube)
        return;

    auto* vkTex = static_cast<VulkanTexture*>(texture);
    if (!vkTex || !vkTex->HasDescriptorSets())
        return;

    const auto& sets = vkTex->GetDescriptorSets();
    if (CurrentFrameIndex < sets.size()) {
        // In the GBuffer geometry pass set 0 is the PBR material descriptor set
        // (m_pbrMaterialDSL, bound by BindPBRMaterial via m_gbufferGeomLayout).
        // Binding a forward single-sampler descriptor set here with the wrong
        // layout would corrupt set 0 and trigger VK_ERROR_DEVICE_LOST.
        // BindPBRMaterial owns set 0 during the geometry pass — skip here.
        if (m_inGeometryPass)
            return;
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &sets[CurrentFrameIndex], 0, nullptr);
    }
}

bool VulkanRenderer::CreateCommandBuffer() {
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commands;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    if (vkAllocateCommandBuffers(device, &allocInfo,
                                  commandBuffers.data()) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to allocate command buffers!");

    return true;
}

void VulkanRenderer::WaitIdle() {
    if (device) vkDeviceWaitIdle(device);
}

void VulkanRenderer::FlushPendingTransfers() {
    m_asyncFlush[currentFrame] = VulkanBuffer::FlushPendingCopiesAsync(
        m_transferSemaphores[currentFrame]);
}

size_t VulkanRenderer::GetGPUMemoryUsed() const {
    return static_cast<size_t>(VulkanBuffer::GetTotalAllocatedBytes());
}

size_t VulkanRenderer::GetGPUMemoryBudget() const {
    return static_cast<size_t>(VulkanBuffer::GetDeviceLocalHeapSize());
}

void VulkanRenderer::Cleanup() {
    SLEAK_INFO("Cleaning Vulkan...");

    bRender = false;

    // Wait for the device to finish all work
    if (device) {
        vkDeviceWaitIdle(device);
    }

    RenderCommandQueue::Shutdown();

    // Flush all deferred buffer deletions now that GPU is idle
    VulkanBuffer::FlushAllDeferredDeletions();

    // Shutdown ImGUI before destroying Vulkan resources
    if (bImInitialized) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        bImInitialized = false;
    }
    if (imguiDescriptorPool) {
        vkDestroyDescriptorPool(device, imguiDescriptorPool, nullptr);
        imguiDescriptorPool = VK_NULL_HANDLE;
    }

    // Destroy descriptor pool (frees descriptor sets too)
    if (descriptorPool) {
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        descriptorPool = VK_NULL_HANDLE;
    }
    descriptorSets.clear();

    // Destroy skybox resources
    if (skyboxPipeline) {
        vkDestroyPipeline(device, skyboxPipeline, nullptr);
        skyboxPipeline = VK_NULL_HANDLE;
    }
    if (skyboxDescriptorPool) {
        vkDestroyDescriptorPool(device, skyboxDescriptorPool, nullptr);
        skyboxDescriptorPool = VK_NULL_HANDLE;
    }
    skyboxDescriptorSets.clear();
    delete skyboxShader;
    skyboxShader = nullptr;

    // Destroy skinned pipeline resources
    if (skinnedPipeline) {
        vkDestroyPipeline(device, skinnedPipeline, nullptr);
        skinnedPipeline = VK_NULL_HANDLE;
    }
    delete skinnedShader;
    skinnedShader = nullptr;

    // Destroy debug line pipeline resources
    if (debugLinePipeline) {
        vkDestroyPipeline(device, debugLinePipeline, nullptr);
        debugLinePipeline = VK_NULL_HANDLE;
    }
    delete debugLineShader;
    debugLineShader = nullptr;

    // Destroy water pipeline resources
    if (m_waterPipeline) {
        vkDestroyPipeline(device, m_waterPipeline, nullptr);
        m_waterPipeline = VK_NULL_HANDLE;
    }
    delete m_waterShader;
    m_waterShader = nullptr;

    // Destroy voxel pipeline resources
    if (m_voxelPipeline) {
        vkDestroyPipeline(device, m_voxelPipeline, nullptr);
        m_voxelPipeline = VK_NULL_HANDLE;
    }
    if (m_gbufferVoxelPipeline) {
        vkDestroyPipeline(device, m_gbufferVoxelPipeline, nullptr);
        m_gbufferVoxelPipeline = VK_NULL_HANDLE;
    }
    if (m_voxelShadowPipeline) {
        vkDestroyPipeline(device, m_voxelShadowPipeline, nullptr);
        m_voxelShadowPipeline = VK_NULL_HANDLE;
    }

    // Destroy MSAA color resources
    CleanupMSAAColorResources();

    // Destroy deferred GBuffer resources
    CleanupGBufferResources();

    // Destroy shadow mapping resources
    CleanupShadowResources();

    // Backstop: free any shader modules whose resource-guarded cleanup was
    // skipped (guard false while shader non-null). Cleanups null after delete,
    // so these are no-ops when already freed — delete(nullptr) is safe.
    delete m_gbufferShader;         m_gbufferShader         = nullptr;
    delete m_lightingShader;        m_lightingShader        = nullptr;
    delete m_ssaoShader;            m_ssaoShader            = nullptr;
    delete m_ssaoBlurShader;        m_ssaoBlurShader        = nullptr;
    delete m_ssrShader;             m_ssrShader             = nullptr;
    delete m_taaShader;             m_taaShader             = nullptr;
    delete m_bloomThresholdShader;  m_bloomThresholdShader  = nullptr;
    delete m_bloomDownsampleShader; m_bloomDownsampleShader = nullptr;
    delete m_bloomUpsampleShader;   m_bloomUpsampleShader   = nullptr;
    delete m_bloomCompositeShader;  m_bloomCompositeShader  = nullptr;

    // Destroy bone UBO resources
    CleanupBoneUBOResources();

    // Destroy descriptor set layouts
    if (m_shadowSamplerDescriptorSetLayout) {
        vkDestroyDescriptorSetLayout(device, m_shadowSamplerDescriptorSetLayout, nullptr);
        m_shadowSamplerDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_lightUBODescriptorSetLayout) {
        vkDestroyDescriptorSetLayout(device, m_lightUBODescriptorSetLayout, nullptr);
        m_lightUBODescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (boneDescriptorSetLayout) {
        vkDestroyDescriptorSetLayout(device, boneDescriptorSetLayout, nullptr);
        boneDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout) {
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        descriptorSetLayout = VK_NULL_HANDLE;
    }

    // Clean up depth resources
    CleanupDepthResources();

    // Destroy swapchain
    if (swapChain && device) {
        vkDestroySwapchainKHR(device, swapChain, nullptr);
        swapChain = VK_NULL_HANDLE;
    }

    // Destroy framebuffers
    for (auto& buffer : swapChainFramebuffers) {
        if (buffer) {
            vkDestroyFramebuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
        }
    }
    swapChainFramebuffers.clear();

    // Destroy image views
    for (auto& imgView : swapChainImageViews) {
        if (imgView) {
            vkDestroyImageView(device, imgView, nullptr);
            imgView = VK_NULL_HANDLE;
        }
    }
    swapChainImageViews.clear();

    // Destroy sync objects
    for (auto& sem : imageAvailableSemaphores) {
        if (sem) vkDestroySemaphore(device, sem, nullptr);
    }
    imageAvailableSemaphores.clear();

    for (auto& sem : renderFinishedSemaphores) {
        if (sem) vkDestroySemaphore(device, sem, nullptr);
    }
    renderFinishedSemaphores.clear();

    for (auto& fence : inFlightFences) {
        if (fence) vkDestroyFence(device, fence, nullptr);
    }
    inFlightFences.clear();

    // Destroy transfer semaphores and free any pending staging buffers
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (m_transferSemaphores[i]) {
            vkDestroySemaphore(device, m_transferSemaphores[i], nullptr);
            m_transferSemaphores[i] = VK_NULL_HANDLE;
        }
        auto& af = m_asyncFlush[i];
        if (af.commandBuffer != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(af.device, af.commandPool, 1,
                                 &af.commandBuffer);
        }
        for (auto& pending : af.stagingBuffers) {
            vmaDestroyBuffer(VulkanBuffer::GetAllocator(), pending.buffer,
                             pending.memory);
            VulkanBuffer::UntrackAllocation(pending.allocSize);
        }
        af = {};
    }

    // Destroy shader
    if (simpleShader) {
        delete simpleShader;
        simpleShader = nullptr;
    }

    // Destroy pipeline
    if (pipeline) {
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }

    // Destroy pipeline layout
    if (pipelineLay) {
        vkDestroyPipelineLayout(device, pipelineLay, nullptr);
        pipelineLay = VK_NULL_HANDLE;
    }

    // Destroy render pass
    if (renderPass) {
        vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
    }

    // Destroy default texture
    if (m_defaultTexture) {
        delete m_defaultTexture;
        m_defaultTexture = nullptr;
    }

    // Destroy command pool (this will also free command buffers)
    if (commands) {
        vkDestroyCommandPool(device, commands, nullptr);
        commands = VK_NULL_HANDLE;
    }

    // Destroy surface after swapchain is gone
    if (surface && instance) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }

    // Destroy debug messenger
    if (debugMessenger && vkDestroyDebugUtilsMessengerEXT) {
        vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        debugMessenger = VK_NULL_HANDLE;
    }

    // Drain any buffers freed during teardown, then destroy the VMA allocator
    // (it must outlive every vmaDestroyBuffer, and both precede vkDestroyDevice).
    VulkanBuffer::FlushAllDeferredDeletions();
    VulkanBuffer::DestroyAllocator();

    // Destroy logical device
    if (device) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }

    // Destroy Vulkan instance
    if (instance) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::Resize(uint32_t width, uint32_t height) {
    if (device) {
        RecreateSwapChain();
    }
}

bool VulkanRenderer::RecreateSwapChain() {
    vkDeviceWaitIdle(device);

    // Cleanup GBuffer BEFORE CleanupSwapChain (which destroys depth image)
    // to avoid dangling image view references in GBuffer framebuffer
    bool hadGBuffer = m_gbufferResourcesCreated;
    CleanupGBufferResources();

    CleanupMSAAColorResources();
    CleanupSwapChain();

    if (!CreateSwapChain()) {
        SLEAK_ERROR("Failed to recreate swap chain!");
        return false;
    }
    if (!CreateImageViews()) {
        SLEAK_ERROR("Failed to recreate image views!");
        return false;
    }
    if (!CreateDepthResources()) {
        SLEAK_ERROR("Failed to recreate depth resources!");
        return false;
    }
    if (!CreateMSAAColorResources()) {
        SLEAK_ERROR("Failed to recreate MSAA color resources!");
        return false;
    }
    if (!CreateFrameBuffer()) {
        SLEAK_ERROR("Failed to recreate framebuffers!");
        return false;
    }

    // Resize imagesInFlight in case swapchain image count changed
    imagesInFlight.resize(swapChainImages.size(), VK_NULL_HANDLE);

    // Recreate GBuffer resources if they were previously created
    if (hadGBuffer && m_deferredEnabled) {
        if (!CreateGBufferResources())
            SLEAK_WARN("RecreateSwapChain: Failed to recreate GBuffer resources!");
    }

    return true;
}

void VulkanRenderer::ApplyMSAAChange() {
    if (!m_msaaChangeRequested)
        return;
    m_msaaChangeRequested = false;

    uint32_t newCount = m_pendingMsaaSampleCount;
    m_msaaSampleCount = newCount;

    // Convert to Vulkan enum
    switch (newCount) {
        case 1: m_msaaSamples = VK_SAMPLE_COUNT_1_BIT; break;
        case 2: m_msaaSamples = VK_SAMPLE_COUNT_2_BIT; break;
        case 4: m_msaaSamples = VK_SAMPLE_COUNT_4_BIT; break;
        case 8: m_msaaSamples = VK_SAMPLE_COUNT_8_BIT; break;
        default: m_msaaSamples = VK_SAMPLE_COUNT_1_BIT; break;
    }

    SLEAK_INFO("Applying MSAA change: {}x", newCount);

    vkDeviceWaitIdle(device);

    // Destroy render pass
    if (renderPass) {
        vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
    }

    // Destroy main-pass pipelines (NOT shadow pipeline)
    if (pipeline) {
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (skyboxPipeline) {
        vkDestroyPipeline(device, skyboxPipeline, nullptr);
        skyboxPipeline = VK_NULL_HANDLE;
    }
    if (skinnedPipeline) {
        vkDestroyPipeline(device, skinnedPipeline, nullptr);
        skinnedPipeline = VK_NULL_HANDLE;
    }
    if (debugLinePipeline) {
        vkDestroyPipeline(device, debugLinePipeline, nullptr);
        debugLinePipeline = VK_NULL_HANDLE;
    }
    if (m_waterPipeline) {
        vkDestroyPipeline(device, m_waterPipeline, nullptr);
        m_waterPipeline = VK_NULL_HANDLE;
    }
    delete m_waterShader;
    m_waterShader = nullptr;

    // Cleanup GBuffer BEFORE swapchain/depth (avoids dangling image view refs)
    CleanupGBufferResources();

    // Shutdown ImGUI
    if (bImInitialized) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        bImInitialized = false;
    }

    // Cleanup swapchain-related resources
    CleanupMSAAColorResources();
    CleanupSwapChain();

    // Recreate everything
    CreateSwapChain();
    CreateImageViews();
    CreateDepthResources();
    CreateMSAAColorResources();
    CreateRenderPass();
    CreateFrameBuffer();
    CreateGraphicsPipeline();
    CreateSkyboxPipeline();
    CreateSkinnedPipeline();
    CreateDebugLinePipeline();
    if (m_deferredEnabled) CreateGBufferResources();
    CreateWaterPipeline();
    CreateVoxelPipeline();
    CreateImGUI();

    // Re-bind skybox cubemap texture to the new descriptor sets
    if (m_skyboxCubemapView != VK_NULL_HANDLE && m_skyboxCubemapSampler != VK_NULL_HANDLE) {
        for (size_t i = 0; i < skyboxDescriptorSets.size(); i++) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = m_skyboxCubemapView;
            imageInfo.sampler = m_skyboxCubemapSampler;

            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = skyboxDescriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
        }
        m_skyboxDescriptorsWritten = true;
    }

    SLEAK_INFO("MSAA change applied successfully");
}

void VulkanRenderer::ApplyVSyncChange() {
    if (!m_vsyncChangeRequested)
        return;
    m_vsyncChangeRequested = false;
    RecreateSwapChain();
    SLEAK_INFO("VSync {}", m_vsync ? "enabled" : "disabled");
}

void VulkanRenderer::ConfigureRenderMode() {
    // Pipeline recreation needed for Vulkan polygon mode changes
}

void VulkanRenderer::ConfigureRenderFace() {
    // Pipeline recreation needed for Vulkan cull mode changes
}

bool VulkanRenderer::CreateCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = QueueIDs.GraphicsIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commands) !=
        VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create command pool!");

    return true;
}

bool VulkanRenderer::CreateSyncObjects() {
    uint32_t imageCount = static_cast<uint32_t>(swapChainImages.size());

    // Semaphores sized to swapchain image count to prevent reuse
    // while the presentation engine still holds a reference.
    imageAvailableSemaphores.resize(imageCount);
    renderFinishedSemaphores.resize(imageCount);
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
    imagesInFlight.resize(imageCount, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    // Create per-swapchain-image semaphores
    for (uint32_t i = 0; i < imageCount; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                               &imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                               &renderFinishedSemaphores[i]) != VK_SUCCESS) {
            SLEAK_RETURN_ERR("Failed to create synchronization objects!");
        }
    }

    // Create per-frame-in-flight fences and transfer semaphores
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                               &m_transferSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr,
                           &inFlightFences[i]) != VK_SUCCESS) {
            SLEAK_RETURN_ERR("Failed to create synchronization objects!");
        }
        m_asyncFlush[i] = {};
    }

    m_semaphoreIndex = 0;
    return true;
}

// Recreate only the extent-dependent shadow objects (image/view/framebuffer)
// at the queued resolution. Samplers, render pass and pipeline are
// extent-independent (dynamic viewport). Keep blocks in sync with
// CreateShadowResources.
void VulkanRenderer::ApplyShadowResolutionChange() {
    if (!m_shadowResChangeRequested) return;
    m_shadowResChangeRequested = false;
    if (!m_shadowResourcesCreated) return;
    if (m_pendingShadowMapResolution == m_shadowMapResolution) return;

    vkDeviceWaitIdle(device);

    vkDestroyFramebuffer(device, m_shadowFramebuffer, nullptr);
    m_shadowFramebuffer = VK_NULL_HANDLE;
    vkDestroyImageView(device, m_shadowImageView, nullptr);
    m_shadowImageView = VK_NULL_HANDLE;
    vkDestroyImage(device, m_shadowImage, nullptr);
    m_shadowImage = VK_NULL_HANDLE;
    vkFreeMemory(device, m_shadowImageMemory, nullptr);
    m_shadowImageMemory = VK_NULL_HANDLE;

    m_shadowMapResolution = m_pendingShadowMapResolution;

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

    if (vkCreateImage(device, &imageInfo, nullptr, &m_shadowImage) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Shadow resolution change: image creation failed");
        m_shadowResourcesCreated = false;
        return;
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
        SLEAK_ERROR("Shadow resolution change: memory allocation failed");
        m_shadowResourcesCreated = false;
        return;
    }
    vkBindImageMemory(device, m_shadowImage, m_shadowImageMemory, 0);

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

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_shadowImageView) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Shadow resolution change: image view creation failed");
        m_shadowResourcesCreated = false;
        return;
    }

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_shadowRenderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_shadowImageView;
    fbInfo.width = m_shadowMapResolution;
    fbInfo.height = m_shadowMapResolution;
    fbInfo.layers = 1;

    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_shadowFramebuffer) !=
        VK_SUCCESS) {
        SLEAK_ERROR("Shadow resolution change: framebuffer creation failed");
        m_shadowResourcesCreated = false;
        return;
    }

    // Initial layout transition — descriptor must be valid pre-first-pass
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

    // Point set-3 descriptors at the new image view
    if (m_lightUBOCreated && m_shadowImageView && m_shadowSampler &&
        m_shadowRawSampler) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VkDescriptorImageInfo compareInfo{};
            compareInfo.imageLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            compareInfo.imageView = m_shadowImageView;
            compareInfo.sampler = m_shadowSampler;

            VkDescriptorImageInfo rawInfo{};
            rawInfo.imageLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            rawInfo.imageView = m_shadowImageView;
            rawInfo.sampler = m_shadowRawSampler;

            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_shadowSamplerDescriptorSets[i];
            writes[0].dstBinding = 0;
            writes[0].dstArrayElement = 0;
            writes[0].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &compareInfo;

            writes[1] = writes[0];
            writes[1].dstBinding = 1;
            writes[1].pImageInfo = &rawInfo;

            vkUpdateDescriptorSets(device,
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }

    SLEAK_INFO("VulkanRenderer: Shadow map resized to {}x{}",
               m_shadowMapResolution, m_shadowMapResolution);
}

// ============================================================
// CreateGBufferResources — top-level orchestrator
// The depth image is created by CreateDepthResources() with SAMPLED_BIT
// already set, so we can share it directly.
// ============================================================
bool VulkanRenderer::CreateGBufferResources() {
    if (m_gbufferResourcesCreated) return true;

    // Create GBuffer color attachment images
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width  = scExtent.width;
        imageInfo.extent.height = scExtent.height;
        imageInfo.extent.depth  = 1;
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.format        = m_gbufferFormats[i];
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &imageInfo, nullptr, &m_gbufferImages[i]) != VK_SUCCESS) {
            SLEAK_ERROR("GBuffer: Failed to create GBuffer image {}!", i);
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, m_gbufferImages[i], &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &m_gbufferMemory[i]) != VK_SUCCESS) {
            SLEAK_ERROR("GBuffer: Failed to allocate GBuffer memory {}!", i);
            return false;
        }
        vkBindImageMemory(device, m_gbufferImages[i], m_gbufferMemory[i], 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image    = m_gbufferImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format   = m_gbufferFormats[i];
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &m_gbufferViews[i]) != VK_SUCCESS) {
            SLEAK_ERROR("GBuffer: Failed to create GBuffer image view {}!", i);
            return false;
        }
    }

    if (!CreateGBufferRenderPass())       { SLEAK_ERROR("GBuffer: render pass failed!");         return false; }
    if (!CreateGBufferFramebuffer())      { SLEAK_ERROR("GBuffer: framebuffer failed!");          return false; }
    // SSAO + bloom must be created BEFORE the lighting/forward render passes
    // and BEFORE CreateGBufferDescriptorSets — the lighting pass framebuffers
    // reference m_hdrSceneView (created by CreateBloomResources) and the
    // GBuffer sampler set 0 binding 7 samples the SSAO blur result.
    if (!CreateSSAOResources())           { SLEAK_ERROR("GBuffer: SSAO resources failed!");        return false; }
    if (!CreateBloomResources())          { SLEAK_ERROR("GBuffer: bloom/HDR resources failed!");   return false; }
    // SSR needs the HDR scene view (created by CreateBloomResources), so it
    // must come after that. The bloom composite pass later samples SSR.
    if (!CreateSSRResources())            { SLEAK_ERROR("GBuffer: SSR resources failed!");         return false; }
    // Prime the disabled-effect fallback images once so the per-frame disabled
    // paths can skip their redundant clears (ssao/ssr/bloom).
    m_ssaoFallbackPrimed  = false;
    m_ssrFallbackPrimed   = false;
    m_bloomFallbackPrimed = false;
    InitDisabledEffectFallbacks();
    if (!CreateGBufferDescriptorSets())   { SLEAK_ERROR("GBuffer: descriptor sets failed!");      return false; }
    if (!CreateDeferredCBResources())     { SLEAK_ERROR("GBuffer: deferred CB failed!");          return false; }
    if (!CreatePBRMaterialResources())    { SLEAK_ERROR("GBuffer: PBR material resources failed!"); return false; }
    if (!CreateIBLResources())            { SLEAK_ERROR("GBuffer: IBL resources failed!");        return false; }
    if (!CreateLightingRenderPass())      { SLEAK_ERROR("GBuffer: lighting RP failed!");          return false; }
    if (!CreateLightingFramebuffers())    { SLEAK_ERROR("GBuffer: lighting FBs failed!");         return false; }
    if (!CreateLightingPipeline())        { SLEAK_ERROR("GBuffer: lighting pipeline failed!");    return false; }
    if (!CreateForwardRenderPass())       { SLEAK_ERROR("GBuffer: forward RP failed!");           return false; }
    if (!CreateForwardFramebuffers())     { SLEAK_ERROR("GBuffer: forward FBs failed!");          return false; }
    if (!CreateGBufferPipeline())         { SLEAK_ERROR("GBuffer: gbuffer pipeline failed!");     return false; }
    if (!CreateSkinnedGbufferPipeline()) { SLEAK_ERROR("GBuffer: skinned gbuffer pipeline failed!"); return false; }

    // Now that SSAO inputs (gNormalRough, gDepth) and SSAO blur
    // descriptor set 0 target are available, write SSAO descriptors.
    UpdateSSAODescriptors();
    // SSR input descriptors depend on m_gbufferViews + m_hdrSceneView, both
    // created above — safe to write now.
    UpdateSSRDescriptors();

    // Recreate forward-pass pipelines so they use m_forwardRenderPass instead
    // of the main renderPass (which may have different attachments when MSAA
    // is active, or an incompatible finalLayout).
    if (m_forwardRenderPass != VK_NULL_HANDLE) {
        if (pipeline)         { vkDestroyPipeline(device, pipeline, nullptr);         pipeline = VK_NULL_HANDLE; }
        if (skyboxPipeline)   { vkDestroyPipeline(device, skyboxPipeline, nullptr);   skyboxPipeline = VK_NULL_HANDLE; }
        if (debugLinePipeline) { vkDestroyPipeline(device, debugLinePipeline, nullptr); debugLinePipeline = VK_NULL_HANDLE; }
        if (skinnedPipeline)  { vkDestroyPipeline(device, skinnedPipeline, nullptr);  skinnedPipeline = VK_NULL_HANDLE; }
        if (m_waterPipeline)  { vkDestroyPipeline(device, m_waterPipeline, nullptr);  m_waterPipeline = VK_NULL_HANDLE; }
        if (m_voxelPipeline)  { vkDestroyPipeline(device, m_voxelPipeline, nullptr);  m_voxelPipeline = VK_NULL_HANDLE; }
        if (m_gbufferVoxelPipeline) { vkDestroyPipeline(device, m_gbufferVoxelPipeline, nullptr); m_gbufferVoxelPipeline = VK_NULL_HANDLE; }
        if (m_skinnedGbufferPipeline) { vkDestroyPipeline(device, m_skinnedGbufferPipeline, nullptr); m_skinnedGbufferPipeline = VK_NULL_HANDLE; }
        if (m_voxelShadowPipeline) { vkDestroyPipeline(device, m_voxelShadowPipeline, nullptr); m_voxelShadowPipeline = VK_NULL_HANDLE; }
        delete m_waterShader;
        m_waterShader = nullptr;
        // Destroy old skybox descriptor pool (CreateSkyboxPipeline allocates new ones)
        if (skyboxDescriptorPool) {
            vkDestroyDescriptorPool(device, skyboxDescriptorPool, nullptr);
            skyboxDescriptorPool = VK_NULL_HANDLE;
        }
        skyboxDescriptorSets.clear();
        delete skyboxShader;
        skyboxShader = nullptr;

        CreateGraphicsPipeline();
        CreateSkyboxPipeline();
        CreateDebugLinePipeline();
        CreateSkinnedPipeline();
        CreateSkinnedGbufferPipeline();

        m_gbufferResourcesCreated = true;

        CreateWaterPipeline();
        CreateVoxelPipeline();

        // Re-bind skybox cubemap to the newly allocated descriptor sets
        if (m_skyboxCubemapView != VK_NULL_HANDLE && m_skyboxCubemapSampler != VK_NULL_HANDLE) {
            for (size_t i = 0; i < skyboxDescriptorSets.size(); i++) {
                VkDescriptorImageInfo imageInfo{};
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfo.imageView   = m_skyboxCubemapView;
                imageInfo.sampler     = m_skyboxCubemapSampler;

                VkWriteDescriptorSet descriptorWrite{};
                descriptorWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrite.dstSet          = skyboxDescriptorSets[i];
                descriptorWrite.dstBinding      = 0;
                descriptorWrite.dstArrayElement = 0;
                descriptorWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptorWrite.descriptorCount = 1;
                descriptorWrite.pImageInfo      = &imageInfo;

                vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
            }
            m_skyboxDescriptorsWritten = true;
        }
    }

    SLEAK_INFO("VulkanRenderer: Deferred GBuffer resources created ({}x{})",
               scExtent.width, scExtent.height);
    return true;
}

void VulkanRenderer::FillFullscreenViewportScissor(VkCommandBuffer cmd, VkExtent2D ext) {
    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = 0.0f;
    vp.width    = static_cast<float>(ext.width);
    vp.height   = static_cast<float>(ext.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.offset = {0, 0};
    sc.extent = ext;
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

// ==================================================================
// ==================== SSAO (HBAO-quality) =========================
// ==================================================================
// Half-resolution hemisphere AO with a 32-sample cosine-weighted kernel,
// 4x4 random rotation tile, and depth-aware bilateral blur.

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

// Prime the disabled-effect fallback images (ssaoBlur=white, ssr=black,
// bloom mip0=black) once after (re)creation, leaving them SHADER_READ_ONLY.
// The per-frame disabled paths then skip their redundant clear — the content
// is static, so re-clearing every frame is pure waste (3 clears/frame).
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

// ==================================================================
// ==================== SSR (Screen-Space Reflections) ==============
// ==================================================================
// Full-resolution view-space ray march with binary search refinement.
// Runs AFTER the forward pass (HDR scene must be lit and in
// SHADER_READ_ONLY_OPTIMAL) and BEFORE the bloom threshold pass — the
// composite pass then additively blends the SSR result into the HDR scene.

bool VulkanRenderer::CreateSSRResources() {
    if (m_ssrResourcesCreated) return true;

    // ---- 1. SSR image (full-res, R16G16B16A16 premultiplied) ----
    {
        VkImageCreateInfo ic{};
        ic.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ic.imageType     = VK_IMAGE_TYPE_2D;
        ic.extent.width  = scExtent.width;
        ic.extent.height = scExtent.height;
        ic.extent.depth  = 1;
        ic.mipLevels     = 1;
        ic.arrayLayers   = 1;
        ic.format        = m_ssrFormat;
        ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // TRANSFER_DST enables vkCmdClearColorImage when SSR is disabled
        // (the composite pass always samples this buffer regardless).
        ic.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ic.samples       = VK_SAMPLE_COUNT_1_BIT;
        ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &ic, nullptr, &m_ssrImage) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateImage failed"); return false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, m_ssrImage, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &alloc, nullptr, &m_ssrMemory) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkAllocateMemory failed ({}B, typeIdx={})", req.size, alloc.memoryTypeIndex); return false;
        }
        vkBindImageMemory(device, m_ssrImage, m_ssrMemory, 0);

        VkImageViewCreateInfo vi{};
        vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image    = m_ssrImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = m_ssrFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &m_ssrView) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateImageView failed"); return false;
        }
    }

    // Linear-clamp sampler — bloom composite samples the SSR buffer.
    {
        VkSamplerCreateInfo ss{};
        ss.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ss.magFilter    = VK_FILTER_LINEAR;
        ss.minFilter    = VK_FILTER_LINEAR;
        ss.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ss.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.minLod       = 0.0f;
        ss.maxLod       = 0.0f;
        if (vkCreateSampler(device, &ss, nullptr, &m_ssrSampler) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateSampler failed"); return false;
        }
    }

    // ---- 2. Render pass (single color attachment) ----
    {
        VkAttachmentDescription att{};
        att.format         = m_ssrFormat;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // we write every pixel
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sp{};
        sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments    = &ref;

        // External dependencies mirror the SSAO render pass — we read from
        // GBuffer/HDR samplers before the pass and the composite samples us
        // after, so we bracket with shader-read-to-color-write / color-write-
        // to-shader-read transitions.
        std::array<VkSubpassDependency, 2> deps{};
        deps[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass      = 0;
        deps[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
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
        rp.pAttachments    = &att;
        rp.subpassCount    = 1;
        rp.pSubpasses      = &sp;
        rp.dependencyCount = static_cast<uint32_t>(deps.size());
        rp.pDependencies   = deps.data();
        if (vkCreateRenderPass(device, &rp, nullptr, &m_ssrRenderPass) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateRenderPass failed"); return false;
        }
    }

    // ---- 3. Framebuffer (single view) ----
    {
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = m_ssrRenderPass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &m_ssrView;
        fb.width           = scExtent.width;
        fb.height          = scExtent.height;
        fb.layers          = 1;
        if (vkCreateFramebuffer(device, &fb, nullptr, &m_ssrFramebuffer) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateFramebuffer failed"); return false;
        }
    }

    // ---- 4. Descriptor set layouts ----
    // Set 0: 5 combined image samplers (gNormalRough, gDepth, gMetalEmit,
    //         gAlbedoAO, sceneHDR). World position reconstructed from depth.
    {
        std::array<VkDescriptorSetLayoutBinding, 5> binds{};
        for (uint32_t i = 0; i < binds.size(); ++i) {
            binds[i].binding         = i;
            binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(binds.size());
        info.pBindings    = binds.data();
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_ssrInputDSL) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateDescriptorSetLayout (input) failed"); return false;
        }
    }
    // Set 1: UBO.
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
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_ssrUboDSL) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateDescriptorSetLayout (ubo) failed"); return false;
        }
    }

    // ---- 5. Descriptor pool + sets ----
    {
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = 6 * MAX_FRAMES_IN_FLIGHT;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[1].descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo pool{};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool.pPoolSizes    = sizes.data();
        pool.maxSets       = 2 * MAX_FRAMES_IN_FLIGHT;
        if (vkCreateDescriptorPool(device, &pool, nullptr, &m_ssrPool) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreateDescriptorPool failed"); return false;
        }

        auto allocSets = [&](VkDescriptorSetLayout dsl, std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>& out) -> bool {
            std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
            layouts.fill(dsl);
            VkDescriptorSetAllocateInfo a{};
            a.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            a.descriptorPool     = m_ssrPool;
            a.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
            a.pSetLayouts        = layouts.data();
            return vkAllocateDescriptorSets(device, &a, out.data()) == VK_SUCCESS;
        };
        if (!allocSets(m_ssrInputDSL, m_ssrInputSets)) {
            SLEAK_ERROR("SSR: vkAllocateDescriptorSets (input) failed"); return false;
        }
        if (!allocSets(m_ssrUboDSL, m_ssrUboSets)) {
            SLEAK_ERROR("SSR: vkAllocateDescriptorSets (ubo) failed"); return false;
        }
    }

    // ---- 6. UBO buffers (host visible coherent) ----
    {
        static constexpr VkDeviceSize uboSize = sizeof(SSRParams);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VkBufferCreateInfo bi{};
            bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size        = uboSize;
            bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bi, nullptr, &m_ssrUboBuffers[i]) != VK_SUCCESS) {
                SLEAK_ERROR("SSR: vkCreateBuffer UBO[{}] failed", i); return false;
            }
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, m_ssrUboBuffers[i], &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize  = req.size;
            alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(device, &alloc, nullptr, &m_ssrUboMemory[i]) != VK_SUCCESS) {
                SLEAK_ERROR("SSR: vkAllocateMemory UBO[{}] failed", i); return false;
            }
            vkBindBufferMemory(device, m_ssrUboBuffers[i], m_ssrUboMemory[i], 0);
            if (vkMapMemory(device, m_ssrUboMemory[i], 0, uboSize, 0, &m_ssrUboMapped[i]) != VK_SUCCESS) return false;

            // Bind UBO to set 1 immediately — input descriptors are written
            // later by UpdateSSRDescriptors() once all source views exist.
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = m_ssrUboBuffers[i];
            bufInfo.offset = 0;
            bufInfo.range  = uboSize;

            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = m_ssrUboSets[i];
            w.dstBinding      = 0;
            w.dstArrayElement = 0;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.descriptorCount = 1;
            w.pBufferInfo     = &bufInfo;
            vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        }
    }

    // ---- 7. Pipeline layout ----
    {
        std::array<VkDescriptorSetLayout, 2> layouts = { m_ssrInputDSL, m_ssrUboDSL };
        VkPipelineLayoutCreateInfo pli{};
        pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = static_cast<uint32_t>(layouts.size());
        pli.pSetLayouts    = layouts.data();
        if (vkCreatePipelineLayout(device, &pli, nullptr, &m_ssrPipelineLayout) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: vkCreatePipelineLayout failed"); return false;
        }
    }

    // ---- 8. Pipeline ----
    {
        m_ssrShader = new VulkanShader(device);
        if (!m_ssrShader->compile("assets/shaders/ssr.vert.spv",
                                   "assets/shaders/ssr.frag.spv")) {
            SLEAK_ERROR("SSR: failed to compile ssr shaders");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[] = {
            m_ssrShader->GetVertexInfo(),
            m_ssrShader->GetFragInfo()
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
        // no depth test / write — full-screen post-process

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
        gpi.pStages             = stages;
        gpi.pVertexInputState   = &vin;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState      = &vps;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState   = &ms;
        gpi.pDepthStencilState  = &dss;
        gpi.pColorBlendState    = &cb;
        gpi.pDynamicState       = &ds;
        gpi.layout              = m_ssrPipelineLayout;
        gpi.renderPass          = m_ssrRenderPass;
        gpi.subpass             = 0;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_ssrPipeline) != VK_SUCCESS) {
            SLEAK_ERROR("SSR: failed to create pipeline");
            return false;
        }
    }

    m_ssrResourcesCreated = true;
    SLEAK_INFO("SSR resources created ({}x{})", scExtent.width, scExtent.height);
    return true;
}

void VulkanRenderer::CleanupSSRResources() {
    if (!m_ssrResourcesCreated) return;

    if (m_ssrPipeline)        { vkDestroyPipeline(device, m_ssrPipeline, nullptr);             m_ssrPipeline = VK_NULL_HANDLE; }
    if (m_ssrPipelineLayout)  { vkDestroyPipelineLayout(device, m_ssrPipelineLayout, nullptr); m_ssrPipelineLayout = VK_NULL_HANDLE; }
    delete m_ssrShader; m_ssrShader = nullptr;

    if (m_ssrFramebuffer)     { vkDestroyFramebuffer(device, m_ssrFramebuffer, nullptr);       m_ssrFramebuffer = VK_NULL_HANDLE; }
    if (m_ssrRenderPass)      { vkDestroyRenderPass(device, m_ssrRenderPass, nullptr);         m_ssrRenderPass = VK_NULL_HANDLE; }

    if (m_ssrPool)            { vkDestroyDescriptorPool(device, m_ssrPool, nullptr);           m_ssrPool = VK_NULL_HANDLE; }
    if (m_ssrInputDSL)        { vkDestroyDescriptorSetLayout(device, m_ssrInputDSL, nullptr);  m_ssrInputDSL = VK_NULL_HANDLE; }
    if (m_ssrUboDSL)          { vkDestroyDescriptorSetLayout(device, m_ssrUboDSL, nullptr);    m_ssrUboDSL = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_ssrUboMapped[i])  { vkUnmapMemory(device, m_ssrUboMemory[i]); m_ssrUboMapped[i] = nullptr; }
        if (m_ssrUboBuffers[i]) { vkDestroyBuffer(device, m_ssrUboBuffers[i], nullptr); m_ssrUboBuffers[i] = VK_NULL_HANDLE; }
        if (m_ssrUboMemory[i])  { vkFreeMemory(device, m_ssrUboMemory[i], nullptr);     m_ssrUboMemory[i] = VK_NULL_HANDLE; }
    }

    if (m_ssrView)     { vkDestroyImageView(device, m_ssrView, nullptr);  m_ssrView = VK_NULL_HANDLE; }
    if (m_ssrImage)    { vkDestroyImage(device, m_ssrImage, nullptr);     m_ssrImage = VK_NULL_HANDLE; }
    if (m_ssrMemory)   { vkFreeMemory(device, m_ssrMemory, nullptr);      m_ssrMemory = VK_NULL_HANDLE; }
    if (m_ssrSampler)  { vkDestroySampler(device, m_ssrSampler, nullptr); m_ssrSampler = VK_NULL_HANDLE; }

    m_ssrResourcesCreated = false;
}

// ==================================================================
// ======================= TAA ======================================
// ==================================================================

bool VulkanRenderer::CreateTAAResources() {
    if (m_taaResourcesCreated) return true;

    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;

    // ---- 1. Two ping-pong history images ----
    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo ic{};
        ic.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ic.imageType     = VK_IMAGE_TYPE_2D;
        ic.extent        = { scExtent.width, scExtent.height, 1 };
        ic.mipLevels     = 1;
        ic.arrayLayers   = 1;
        ic.format        = fmt;
        ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ic.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ic.samples       = VK_SAMPLE_COUNT_1_BIT;
        ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &ic, nullptr, &m_taaImages[i]) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, m_taaImages[i], &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &alloc, nullptr, &m_taaMemory[i]) != VK_SUCCESS) return false;
        vkBindImageMemory(device, m_taaImages[i], m_taaMemory[i], 0);

        VkImageViewCreateInfo vi{};
        vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image            = m_taaImages[i];
        vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vi.format           = fmt;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device, &vi, nullptr, &m_taaViews[i]) != VK_SUCCESS) return false;
    }

    // ---- 2. Initialize both images to SHADER_READ_ONLY (cleared black) ----
    {
        VkCommandBufferAllocateInfo ca{};
        ca.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ca.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandPool        = commands;
        ca.commandBufferCount = 1;
        VkCommandBuffer initCmd;
        vkAllocateCommandBuffers(device, &ca, &initCmd);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(initCmd, &bi);

        VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (int i = 0; i < 2; ++i) {
            VkImageMemoryBarrier bar{};
            bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.srcAccessMask       = 0;
            bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.image               = m_taaImages[i];
            bar.subresourceRange    = sr;
            vkCmdPipelineBarrier(initCmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &bar);
        }
        VkClearColorValue black{};
        for (int i = 0; i < 2; ++i)
            vkCmdClearColorImage(initCmd, m_taaImages[i],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &sr);
        for (int i = 0; i < 2; ++i) {
            VkImageMemoryBarrier bar{};
            bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            bar.image               = m_taaImages[i];
            bar.subresourceRange    = sr;
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
    }

    // ---- 3. Linear-clamp sampler ----
    {
        VkSamplerCreateInfo ss{};
        ss.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ss.magFilter    = VK_FILTER_LINEAR;
        ss.minFilter    = VK_FILTER_LINEAR;
        ss.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ss.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ss.minLod = 0.0f; ss.maxLod = 0.0f;
        if (vkCreateSampler(device, &ss, nullptr, &m_taaSampler) != VK_SUCCESS) return false;
    }

    // ---- 4. Render pass (shared for both ping-pong targets) ----
    {
        VkAttachmentDescription att{};
        att.format         = fmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 1;
        rpi.pAttachments    = &att;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sub;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        if (vkCreateRenderPass(device, &rpi, nullptr, &m_taaRenderPass) != VK_SUCCESS) return false;
    }

    // ---- 5. Two framebuffers (one per ping-pong target) ----
    for (int i = 0; i < 2; ++i) {
        VkFramebufferCreateInfo fi{};
        fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = m_taaRenderPass;
        fi.attachmentCount = 1;
        fi.pAttachments    = &m_taaViews[i];
        fi.width           = scExtent.width;
        fi.height          = scExtent.height;
        fi.layers          = 1;
        if (vkCreateFramebuffer(device, &fi, nullptr, &m_taaFramebufs[i]) != VK_SUCCESS) return false;
    }

    // ---- 6. Descriptor set layouts ----
    // set 0: 3 combined image samplers (currentTex, historyTex, gDepth)
    {
        std::array<VkDescriptorSetLayoutBinding, 3> binds{};
        for (uint32_t b = 0; b < 3; ++b) {
            binds[b].binding         = b;
            binds[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[b].descriptorCount = 1;
            binds[b].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = static_cast<uint32_t>(binds.size());
        li.pBindings    = binds.data();
        if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_taaInputDSL) != VK_SUCCESS) return false;
    }
    // set 1: UBO
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings    = &b;
        if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_taaUboDSL) != VK_SUCCESS) return false;
    }

    // ---- 7. Descriptor pool + sets ----
    {
        std::array<VkDescriptorPoolSize, 2> ps{};
        ps[0] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * MAX_FRAMES_IN_FLIGHT };
        ps[1] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1 * MAX_FRAMES_IN_FLIGHT };
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.poolSizeCount = static_cast<uint32_t>(ps.size());
        pi.pPoolSizes    = ps.data();
        pi.maxSets       = 2 * MAX_FRAMES_IN_FLIGHT;
        if (vkCreateDescriptorPool(device, &pi, nullptr, &m_taaPool) != VK_SUCCESS) return false;

        for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = m_taaPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &m_taaInputDSL;
            if (vkAllocateDescriptorSets(device, &ai, &m_taaInputSets[f]) != VK_SUCCESS) return false;

            ai.pSetLayouts = &m_taaUboDSL;
            if (vkAllocateDescriptorSets(device, &ai, &m_taaUboSets[f]) != VK_SUCCESS) return false;
        }
    }

    // ---- 8. UBO buffers (HOST_VISIBLE | HOST_COHERENT) ----
    {
        const VkDeviceSize uboSize = sizeof(TAAParams);
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = uboSize;
        bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
            if (vkCreateBuffer(device, &bi, nullptr, &m_taaUboBuffers[f]) != VK_SUCCESS) return false;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, m_taaUboBuffers[f], &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(device, &alloc, nullptr, &m_taaUboMemory[f]) != VK_SUCCESS) return false;
            vkBindBufferMemory(device, m_taaUboBuffers[f], m_taaUboMemory[f], 0);
            vkMapMemory(device, m_taaUboMemory[f], 0, uboSize, 0, &m_taaUboMapped[f]);
            memset(m_taaUboMapped[f], 0, uboSize);

            // Wire UBO descriptor
            VkDescriptorBufferInfo dbi{ m_taaUboBuffers[f], 0, uboSize };
            VkWriteDescriptorSet wr{};
            wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet          = m_taaUboSets[f];
            wr.dstBinding      = 0;
            wr.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr.descriptorCount = 1;
            wr.pBufferInfo     = &dbi;
            vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
        }
    }

    // ---- 9. Pipeline layout ----
    {
        std::array<VkDescriptorSetLayout, 2> layouts{ m_taaInputDSL, m_taaUboDSL };
        VkPipelineLayoutCreateInfo pli{};
        pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = static_cast<uint32_t>(layouts.size());
        pli.pSetLayouts    = layouts.data();
        if (vkCreatePipelineLayout(device, &pli, nullptr, &m_taaPipelineLayout) != VK_SUCCESS) return false;
    }

    // ---- 10. Pipeline ----
    {
        m_taaShader = new VulkanShader(device);
        if (!m_taaShader->compile("assets/shaders/taa.vert.spv",
                                   "assets/shaders/taa.frag.spv")) {
            SLEAK_ERROR("TAA: failed to compile shaders");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[] = {
            m_taaShader->GetVertexInfo(),
            m_taaShader->GetFragInfo()
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
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

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
        gpi.pStages             = stages;
        gpi.pVertexInputState   = &vin;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState      = &vps;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState   = &ms;
        gpi.pDepthStencilState  = &dss;
        gpi.pColorBlendState    = &cb;
        gpi.pDynamicState       = &ds;
        gpi.layout              = m_taaPipelineLayout;
        gpi.renderPass          = m_taaRenderPass;
        gpi.subpass             = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr,
                                       &m_taaPipeline) != VK_SUCCESS) {
            SLEAK_ERROR("TAA: failed to create pipeline");
            return false;
        }
    }

    m_taaResourcesCreated = true;
    SLEAK_INFO("TAA resources created ({}x{})", scExtent.width, scExtent.height);
    return true;
}

void VulkanRenderer::CleanupTAAResources() {
    if (!m_taaResourcesCreated) return;

    vkDeviceWaitIdle(device);

    if (m_taaPipeline)       { vkDestroyPipeline(device, m_taaPipeline, nullptr);             m_taaPipeline = VK_NULL_HANDLE; }
    if (m_taaPipelineLayout) { vkDestroyPipelineLayout(device, m_taaPipelineLayout, nullptr); m_taaPipelineLayout = VK_NULL_HANDLE; }
    delete m_taaShader; m_taaShader = nullptr;

    for (int i = 0; i < 2; ++i) {
        if (m_taaFramebufs[i]) { vkDestroyFramebuffer(device, m_taaFramebufs[i], nullptr); m_taaFramebufs[i] = VK_NULL_HANDLE; }
    }
    if (m_taaRenderPass) { vkDestroyRenderPass(device, m_taaRenderPass, nullptr); m_taaRenderPass = VK_NULL_HANDLE; }

    if (m_taaPool)     { vkDestroyDescriptorPool(device, m_taaPool, nullptr);           m_taaPool = VK_NULL_HANDLE; }
    if (m_taaInputDSL) { vkDestroyDescriptorSetLayout(device, m_taaInputDSL, nullptr);  m_taaInputDSL = VK_NULL_HANDLE; }
    if (m_taaUboDSL)   { vkDestroyDescriptorSetLayout(device, m_taaUboDSL, nullptr);    m_taaUboDSL = VK_NULL_HANDLE; }

    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        if (m_taaUboMapped[f])   { vkUnmapMemory(device, m_taaUboMemory[f]); m_taaUboMapped[f] = nullptr; }
        if (m_taaUboBuffers[f])  { vkDestroyBuffer(device, m_taaUboBuffers[f], nullptr); m_taaUboBuffers[f] = VK_NULL_HANDLE; }
        if (m_taaUboMemory[f])   { vkFreeMemory(device, m_taaUboMemory[f], nullptr);     m_taaUboMemory[f] = VK_NULL_HANDLE; }
    }

    if (m_taaSampler) { vkDestroySampler(device, m_taaSampler, nullptr); m_taaSampler = VK_NULL_HANDLE; }

    for (int i = 0; i < 2; ++i) {
        if (m_taaViews[i])  { vkDestroyImageView(device, m_taaViews[i], nullptr);  m_taaViews[i] = VK_NULL_HANDLE; }
        if (m_taaImages[i]) { vkDestroyImage(device, m_taaImages[i], nullptr);      m_taaImages[i] = VK_NULL_HANDLE; }
        if (m_taaMemory[i]) { vkFreeMemory(device, m_taaMemory[i], nullptr);        m_taaMemory[i] = VK_NULL_HANDLE; }
    }

    m_taaResourcesCreated = false;
}

void VulkanRenderer::UpdateTAAUBO() {
    if (!m_taaResourcesCreated || !m_taaUboMapped[currentFrame]) return;

    // VP = View * Projection (row-major)
    float currentVP[16];
    MatMul4(m_cachedView, m_cachedProjection, currentVP);

    float invCurrentVP[16];
    if (!InvertMat4(currentVP, invCurrentVP)) {
        memset(invCurrentVP, 0, sizeof(invCurrentVP));
        for (int i = 0; i < 4; ++i) invCurrentVP[i * 4 + i] = 1.0f;  // identity fallback
    }

    TAAParams p{};
    memcpy(p.InvCurrentVP, invCurrentVP, sizeof(p.InvCurrentVP));
    memcpy(p.PrevVP,       m_prevViewProj, sizeof(p.PrevVP));
    p.ScreenW     = static_cast<float>(scExtent.width);
    p.ScreenH     = static_cast<float>(scExtent.height);
    // First two frames skip history (prev VP is zero-initialized)
    p.BlendFactor = (m_taaFrameIdx <= 1) ? 1.0f : 0.1f;
    p._pad        = 0.0f;

    memcpy(m_taaUboMapped[currentFrame], &p, sizeof(p));

    // Save current VP as previous for next frame
    memcpy(m_prevViewProj, currentVP, sizeof(m_prevViewProj));
}

void VulkanRenderer::RenderTAAPass() {
    if (!m_taaResourcesCreated || !m_taaEnabled) return;

    const uint32_t writeIdx = static_cast<uint32_t>(m_taaFrameIdx % 2);
    const uint32_t readIdx  = 1u - writeIdx;

    // ---- 1. Depth barrier: DEPTH_STENCIL_ATTACHMENT → READ_ONLY ----
    // (SSR will skip its own depth barrier since TAA already handles it)
    {
        VkImageMemoryBarrier depBar{};
        depBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depBar.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depBar.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depBar.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depBar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        depBar.image               = depthImage;
        depBar.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &depBar);
    }

    // ---- 2. Transition write target: SHADER_READ_ONLY → COLOR_ATTACHMENT ----
    {
        VkImageMemoryBarrier bar{};
        bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        bar.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.image               = m_taaImages[writeIdx];
        bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    // ---- 3. Update input descriptors for this frame ----
    {
        // binding 0: currentTex = hdrScene (already SHADER_READ_ONLY from forward pass)
        // binding 1: historyTex = taaImages[readIdx] (SHADER_READ_ONLY from init / prev frame)
        // binding 2: gDepth     (now DEPTH_STENCIL_READ_ONLY from step 1)
        VkDescriptorImageInfo infos[3]{};
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = m_hdrSceneView;
        infos[0].sampler     = m_taaSampler;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[1].imageView   = m_taaViews[readIdx];
        infos[1].sampler     = m_taaSampler;
        infos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[2].imageView   = depthImageView;
        infos[2].sampler     = m_taaSampler;

        VkWriteDescriptorSet writes[3]{};
        for (int b = 0; b < 3; ++b) {
            writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet          = m_taaInputSets[currentFrame];
            writes[b].dstBinding      = static_cast<uint32_t>(b);
            writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].descriptorCount = 1;
            writes[b].pImageInfo      = &infos[b];
        }
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    }

    // ---- 4. Update UBO ----
    UpdateTAAUBO();

    // ---- 5. TAA render pass ----
    {
        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_taaRenderPass;
        rp.framebuffer       = m_taaFramebufs[writeIdx];
        rp.renderArea.offset = { 0, 0 };
        rp.renderArea.extent = scExtent;
        rp.clearValueCount   = 0;
        vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
        FillFullscreenViewportScissor(command, scExtent);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipeline);
        VkDescriptorSet sets[2] = { m_taaInputSets[currentFrame], m_taaUboSets[currentFrame] };
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_taaPipelineLayout, 0, 2, sets, 0, nullptr);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        // taaImages[writeIdx] is now SHADER_READ_ONLY_OPTIMAL (render pass finalLayout)
    }

    // ---- 6. Copy TAA resolve → hdrScene ----
    // Both images are SHADER_READ_ONLY; transition for transfer then restore.
    {
        VkImageMemoryBarrier toTransfer[2]{};
        toTransfer[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer[0].oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransfer[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer[0].srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        toTransfer[0].dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer[0].image               = m_taaImages[writeIdx];
        toTransfer[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        toTransfer[1]             = toTransfer[0];
        toTransfer[1].newLayout   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer[1].image       = m_hdrSceneImage;

        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, toTransfer);

        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent         = { scExtent.width, scExtent.height, 1 };
        vkCmdCopyImage(command,
            m_taaImages[writeIdx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_hdrSceneImage,       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &region);

        VkImageMemoryBarrier toRead[2]{};
        toRead[0]              = toTransfer[0];
        toRead[0].oldLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead[0].newLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toRead[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        toRead[1]              = toRead[0];
        toRead[1].oldLayout    = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead[1].image        = m_hdrSceneImage;

        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 2, toRead);
    }

    // Advance ping-pong for next frame
    m_taaFrameIdx++;
}

void VulkanRenderer::UpdateSSRDescriptors() {
    if (!m_ssrResourcesCreated) return;

    // Write the input samplers for every frame slot. Called during init, so
    // no concurrent GPU access — safe to update both slots at once.
    // Sampler slots: [0]=gNormalRough, [1]=gDepth, [2]=gMetalEmit,
    //                [3]=gAlbedoAO, [4]=sceneHDR. World pos from depth.
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        std::array<VkDescriptorImageInfo, 5> infos{};

        // gNormalRough = gbuffer[1]
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = m_gbufferViews[1];
        infos[0].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // gDepth — READ_ONLY because shadow pass finalLayout is
        // DEPTH_STENCIL_READ_ONLY, GBuffer pass final is READ_ONLY, but the
        // forward pass exits at DEPTH_STENCIL_ATTACHMENT_OPTIMAL. SSR bracket
        // transitions it to READ_ONLY before sampling (see RenderSSRPass).
        infos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[1].imageView   = depthImageView;
        infos[1].sampler     = m_depthSampler ? m_depthSampler : m_ssrSampler;

        // gMetalEmit = gbuffer[2]
        infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[2].imageView   = m_gbufferViews[2];
        infos[2].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // gAlbedoAO = gbuffer[0]
        infos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[3].imageView   = m_gbufferViews[0];
        infos[3].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // sceneHDR — forward pass finalLayout is SHADER_READ_ONLY_OPTIMAL.
        infos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[4].imageView   = m_hdrSceneView;
        infos[4].sampler     = m_ssrSampler;

        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = m_ssrInputSets[f];
            writes[i].dstBinding      = i;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void VulkanRenderer::UpdateSSRUBO() {
    if (!m_ssrResourcesCreated || !m_ssrUboMapped[currentFrame]) return;

    SSRParams p{};
    memcpy(p.View,        m_cachedView,        sizeof(p.View));
    memcpy(p.Projection,  m_cachedProjection,  sizeof(p.Projection));
    memcpy(p.InvViewProj, m_cachedInvViewProj, sizeof(p.InvViewProj));

    const auto& camPos = Camera::GetMainCameraPosition();
    p.CameraPos[0] = camPos.GetX();
    p.CameraPos[1] = camPos.GetY();
    p.CameraPos[2] = camPos.GetZ();
    p.CameraPos[3] = 0.0f;

    p.ScreenW = static_cast<float>(scExtent.width);
    p.ScreenH = static_cast<float>(scExtent.height);

    // Quality vs perf defaults — 32 coarse + 8 binary is the sweet spot
    // for full-res UE-style SSR. Thickness in view-space *depth* units
    // (post-divide), 0.02 catches near+mid hits without smearing through
    // thin geometry.
    p.MaxDistance        = 20.0f;
    p.Thickness          = 0.5f;
    p.NumSteps           = 20;
    p.NumBinarySteps     = 6;
    p.RoughnessThreshold = 0.9f;
    p._pad               = 0.0f;

    memcpy(m_ssrUboMapped[currentFrame], &p, sizeof(p));
}

void VulkanRenderer::RenderSSRPass() {
    if (!m_ssrResourcesCreated) return;

    // Depth must be in DEPTH_STENCIL_READ_ONLY_OPTIMAL before SSR samples it.
    // When TAA is enabled (and ran before SSR), it already issued this barrier.
    // When TAA is disabled, do it here instead.
    if (!m_taaResourcesCreated || !m_taaEnabled) {
        VkImageMemoryBarrier depthBarrier{};
        depthBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthBarrier.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthBarrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        depthBarrier.image               = depthImage;
        depthBarrier.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
    }

    // When SSR is disabled we still need the reflection buffer in a defined
    // state for the composite pass. The cheapest way is a single render-pass
    // begin that (with DONT_CARE load) transitions UNDEFINED -> SHADER_READ_ONLY
    // via finalLayout — but we also need actual zeroed contents. Use a
    // vkCmdClearColorImage instead (image is in UNDEFINED -> TRANSFER_DST ->
    // SHADER_READ_ONLY). Since the image is re-defined each frame the
    // UNDEFINED initial layout is fine.
    if (!m_ssrEnabled) {
        // Already primed to black SHADER_READ_ONLY — skip the redundant
        // per-frame clear. Re-prime only if the enabled path dirtied it.
        if (m_ssrFallbackPrimed) return;
        VkImageMemoryBarrier toClear{};
        toClear.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toClear.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toClear.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.srcAccessMask       = 0;
        toClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toClear.image               = m_ssrImage;
        toClear.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toClear);

        VkClearColorValue black{};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, m_ssrImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &black, 1, &range);

        VkImageMemoryBarrier toRead = toClear;
        toRead.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toRead);
        m_ssrFallbackPrimed = true;
        return;
    }

    // Enabled path dirties the SSR image; force a re-prime if SSR is later
    // disabled so the composite doesn't sample stale reflections.
    m_ssrFallbackPrimed = false;

    UpdateSSRUBO();

    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = m_ssrRenderPass;
    rp.framebuffer       = m_ssrFramebuffer;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = scExtent;
    rp.clearValueCount   = 0;  // DONT_CARE load — we write every pixel

    vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    FillFullscreenViewportScissor(command, scExtent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssrPipeline);

    VkDescriptorSet sets[2] = { m_ssrInputSets[currentFrame], m_ssrUboSets[currentFrame] };
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_ssrPipelineLayout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
}

// ==================================================================
// ==================== Bloom (UE4-style pyramid) ===================
// ==================================================================

bool VulkanRenderer::CreateHDRSceneResources() {
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent.width  = scExtent.width;
    info.extent.height = scExtent.height;
    info.extent.depth  = 1;
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.format        = m_hdrSceneFormat;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // TRANSFER_DST_BIT: TAA copies its resolved result back into hdrScene.
    // TRANSFER_SRC_BIT: MSAA resolve from the forward render pass.
    info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &info, nullptr, &m_hdrSceneImage) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, m_hdrSceneImage, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &m_hdrSceneMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, m_hdrSceneImage, m_hdrSceneMemory, 0);

    VkImageViewCreateInfo vinfo{};
    vinfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vinfo.image    = m_hdrSceneImage;
    vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vinfo.format   = m_hdrSceneFormat;
    vinfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vinfo, nullptr, &m_hdrSceneView) != VK_SUCCESS) return false;

    // Shared linear-clamp sampler for every bloom source sample.
    VkSamplerCreateInfo sinfo{};
    sinfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sinfo.magFilter    = VK_FILTER_LINEAR;
    sinfo.minFilter    = VK_FILTER_LINEAR;
    sinfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.minLod       = 0.0f;
    sinfo.maxLod       = 0.0f;
    if (vkCreateSampler(device, &sinfo, nullptr, &m_bloomSampler) != VK_SUCCESS) return false;

    return true;
}

bool VulkanRenderer::CreateBloomImages() {
    // Single image with BLOOM_MIP_COUNT mip levels, each starting at
    // (scExtent / 2) and halving. HDR float format preserves bloom energy.
    uint32_t w = std::max(1u, scExtent.width  / 2);
    uint32_t h = std::max(1u, scExtent.height / 2);

    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent.width  = w;
    info.extent.height = h;
    info.extent.depth  = 1;
    info.mipLevels     = BLOOM_MIP_COUNT;
    info.arrayLayers   = 1;
    info.format        = m_hdrSceneFormat;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // TRANSFER_DST_BIT: when bloom is disabled, mip[0] is cleared-to-black so the
    // composite pass samples a defined SHADER_READ_ONLY image (no full mip chain).
    info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &info, nullptr, &m_bloomImage) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, m_bloomImage, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &m_bloomMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, m_bloomImage, m_bloomMemory, 0);

    // Per-mip views — each is a single-mip view so framebuffers can target it.
    for (uint32_t m = 0; m < BLOOM_MIP_COUNT; ++m) {
        m_bloomMipExtents[m].width  = std::max(1u, w >> m);
        m_bloomMipExtents[m].height = std::max(1u, h >> m);

        VkImageViewCreateInfo vinfo{};
        vinfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vinfo.image    = m_bloomImage;
        vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vinfo.format   = m_hdrSceneFormat;
        vinfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
        if (vkCreateImageView(device, &vinfo, nullptr, &m_bloomMipViews[m]) != VK_SUCCESS) return false;
    }

    return true;
}

bool VulkanRenderer::CreateBloomRenderPasses() {
    // Common HDR color attachment.
    auto makeRP = [&](VkAttachmentLoadOp loadOp, VkImageLayout initial,
                      VkFormat fmt, VkImageLayout finalLayout,
                      VkRenderPass& out) -> bool {
        VkAttachmentDescription att{};
        att.format         = fmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = loadOp;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = initial;
        att.finalLayout    = finalLayout;

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sp{};
        sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments    = &ref;

        std::array<VkSubpassDependency, 2> d{};
        d[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        d[0].dstSubpass      = 0;
        d[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        d[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        d[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        d[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        d[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        d[1].srcSubpass      = 0;
        d[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        d[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        d[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        d[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        d[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        d[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo rp{};
        rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 1;
        rp.pAttachments    = &att;
        rp.subpassCount    = 1;
        rp.pSubpasses      = &sp;
        rp.dependencyCount = static_cast<uint32_t>(d.size());
        rp.pDependencies   = d.data();

        return vkCreateRenderPass(device, &rp, nullptr, &out) == VK_SUCCESS;
    };

    // Downsample / threshold: DONT_CARE → STORE → SHADER_READ_ONLY.
    if (!makeRP(VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                m_hdrSceneFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                m_bloomRenderPass)) return false;

    // Upsample (blend-add): LOAD → STORE → SHADER_READ_ONLY.
    if (!makeRP(VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                m_hdrSceneFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                m_bloomAddRenderPass)) return false;

    // Composite: writes swapchain image, ends in PRESENT_SRC_KHR. ImGui draws here.
    if (!makeRP(VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                scImageFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                m_bloomCompositeRenderPass)) return false;

    return true;
}

bool VulkanRenderer::CreateBloomFramebuffers() {
    // One framebuffer per bloom mip (reused between threshold/downsample/upsample).
    for (uint32_t m = 0; m < BLOOM_MIP_COUNT; ++m) {
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = m_bloomRenderPass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &m_bloomMipViews[m];
        fb.width           = m_bloomMipExtents[m].width;
        fb.height          = m_bloomMipExtents[m].height;
        fb.layers          = 1;
        if (vkCreateFramebuffer(device, &fb, nullptr, &m_bloomMipFramebuffers[m]) != VK_SUCCESS) return false;
    }

    // One framebuffer per swapchain image for the composite pass.
    m_bloomCompositeFramebuffers.resize(swapChainImageViews.size());
    for (size_t i = 0; i < swapChainImageViews.size(); ++i) {
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = m_bloomCompositeRenderPass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &swapChainImageViews[i];
        fb.width           = scExtent.width;
        fb.height          = scExtent.height;
        fb.layers          = 1;
        if (vkCreateFramebuffer(device, &fb, nullptr, &m_bloomCompositeFramebuffers[i]) != VK_SUCCESS) return false;
    }

    return true;
}

bool VulkanRenderer::CreateBloomDescriptorResources() {
    // Filter DSL: single combined image sampler (binding 0).
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings    = &b;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_bloomFilterDSL) != VK_SUCCESS) return false;
    }
    // Composite DSL: three combined image samplers (sceneHDR + bloom + SSR).
    {
        std::array<VkDescriptorSetLayoutBinding, 3> binds{};
        for (uint32_t i = 0; i < binds.size(); ++i) {
            binds[i].binding         = i;
            binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(binds.size());
        info.pBindings    = binds.data();
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_bloomCompositeDSL) != VK_SUCCESS) return false;
    }

    // Pool sized for BLOOM_TRANSITION_COUNT filter sets per frame slot + 1 composite set per frame.
    // Composite set now binds 3 samplers (HDR + bloom + SSR).
    VkDescriptorPoolSize sz{};
    sz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sz.descriptorCount = (BLOOM_TRANSITION_COUNT + 3) * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.poolSizeCount = 1;
    pool.pPoolSizes    = &sz;
    pool.maxSets       = (BLOOM_TRANSITION_COUNT + 1) * MAX_FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &m_bloomDescriptorPool) != VK_SUCCESS) return false;

    // Allocate filter sets: MAX_FRAMES_IN_FLIGHT frames × BLOOM_TRANSITION_COUNT transitions.
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        std::array<VkDescriptorSetLayout, BLOOM_TRANSITION_COUNT> layouts;
        layouts.fill(m_bloomFilterDSL);
        VkDescriptorSetAllocateInfo a{};
        a.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        a.descriptorPool     = m_bloomDescriptorPool;
        a.descriptorSetCount = BLOOM_TRANSITION_COUNT;
        a.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(device, &a, m_bloomFilterSets[f].data()) != VK_SUCCESS) return false;
    }

    // Allocate composite sets (one per frame).
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_bloomCompositeDSL);
    VkDescriptorSetAllocateInfo a{};
    a.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    a.descriptorPool     = m_bloomDescriptorPool;
    a.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    a.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(device, &a, m_bloomCompositeSets.data()) != VK_SUCCESS) return false;

    return true;
}

bool VulkanRenderer::CreateBloomPipelines() {
    // ---- Pipeline layouts ----
    VkPushConstantRange filterPushRange{};
    filterPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    filterPushRange.offset     = 0;
    filterPushRange.size       = 16; // 4 floats, all filter PCs are 16 bytes

    VkPipelineLayoutCreateInfo fliInfo{};
    fliInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    fliInfo.setLayoutCount         = 1;
    fliInfo.pSetLayouts            = &m_bloomFilterDSL;
    fliInfo.pushConstantRangeCount = 1;
    fliInfo.pPushConstantRanges    = &filterPushRange;
    if (vkCreatePipelineLayout(device, &fliInfo, nullptr, &m_bloomFilterPipelineLayout) != VK_SUCCESS) return false;

    VkPushConstantRange compPushRange{};
    compPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    compPushRange.offset     = 0;
    compPushRange.size       = 16;

    VkPipelineLayoutCreateInfo cliInfo{};
    cliInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    cliInfo.setLayoutCount         = 1;
    cliInfo.pSetLayouts            = &m_bloomCompositeDSL;
    cliInfo.pushConstantRangeCount = 1;
    cliInfo.pPushConstantRanges    = &compPushRange;
    if (vkCreatePipelineLayout(device, &cliInfo, nullptr, &m_bloomCompositePipelineLayout) != VK_SUCCESS) return false;

    // ---- Compile shaders ----
    m_bloomThresholdShader = new VulkanShader(device);
    if (!m_bloomThresholdShader->compile("assets/shaders/bloom.vert.spv",
                                         "assets/shaders/bloom_threshold.frag.spv")) return false;
    m_bloomDownsampleShader = new VulkanShader(device);
    if (!m_bloomDownsampleShader->compile("assets/shaders/bloom.vert.spv",
                                          "assets/shaders/bloom_downsample.frag.spv")) return false;
    m_bloomUpsampleShader = new VulkanShader(device);
    if (!m_bloomUpsampleShader->compile("assets/shaders/bloom.vert.spv",
                                        "assets/shaders/bloom_upsample.frag.spv")) return false;
    m_bloomCompositeShader = new VulkanShader(device);
    if (!m_bloomCompositeShader->compile("assets/shaders/bloom.vert.spv",
                                         "assets/shaders/bloom_composite.frag.spv")) return false;

    // ---- Common pipeline state (fullscreen triangle) ----
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

    // Opaque blend (default).
    VkPipelineColorBlendAttachmentState blendOpaque{};
    blendOpaque.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbOpaque{};
    cbOpaque.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbOpaque.attachmentCount = 1;
    cbOpaque.pAttachments    = &blendOpaque;

    // Additive blend for bloom upsample.
    VkPipelineColorBlendAttachmentState blendAdd{};
    blendAdd.blendEnable         = VK_TRUE;
    blendAdd.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAdd.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAdd.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAdd.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAdd.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAdd.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAdd.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbAdd{};
    cbAdd.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbAdd.attachmentCount = 1;
    cbAdd.pAttachments    = &blendAdd;

    // ---- Threshold pipeline (writes mip 0 of bloom image) ----
    VkPipelineShaderStageCreateInfo threshStages[] = {
        m_bloomThresholdShader->GetVertexInfo(),
        m_bloomThresholdShader->GetFragInfo()
    };
    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount          = 2;
    gpi.pStages             = threshStages;
    gpi.pVertexInputState   = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &dss;
    gpi.pColorBlendState    = &cbOpaque;
    gpi.pDynamicState       = &ds;
    gpi.layout              = m_bloomFilterPipelineLayout;
    gpi.renderPass          = m_bloomRenderPass;
    gpi.subpass             = 0;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_bloomThresholdPipeline) != VK_SUCCESS) return false;

    // ---- Downsample pipeline ----
    VkPipelineShaderStageCreateInfo downStages[] = {
        m_bloomDownsampleShader->GetVertexInfo(),
        m_bloomDownsampleShader->GetFragInfo()
    };
    gpi.pStages = downStages;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_bloomDownsamplePipeline) != VK_SUCCESS) return false;

    // ---- Upsample pipeline (additive blend, LOAD render pass) ----
    VkPipelineShaderStageCreateInfo upStages[] = {
        m_bloomUpsampleShader->GetVertexInfo(),
        m_bloomUpsampleShader->GetFragInfo()
    };
    gpi.pStages          = upStages;
    gpi.pColorBlendState = &cbAdd;
    gpi.renderPass       = m_bloomAddRenderPass;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_bloomUpsamplePipeline) != VK_SUCCESS) return false;

    // ---- Composite pipeline ----
    VkPipelineShaderStageCreateInfo compStages[] = {
        m_bloomCompositeShader->GetVertexInfo(),
        m_bloomCompositeShader->GetFragInfo()
    };
    gpi.pStages          = compStages;
    gpi.pColorBlendState = &cbOpaque;
    gpi.layout           = m_bloomCompositePipelineLayout;
    gpi.renderPass       = m_bloomCompositeRenderPass;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_bloomCompositePipeline) != VK_SUCCESS) return false;

    return true;
}

bool VulkanRenderer::CreateBloomResources() {
    if (m_bloomResourcesCreated) return true;

    if (!CreateHDRSceneResources())        { SLEAK_ERROR("Bloom: HDR scene resources failed");  return false; }
    if (!CreateBloomImages())              { SLEAK_ERROR("Bloom: images failed");               return false; }
    if (!CreateBloomRenderPasses())        { SLEAK_ERROR("Bloom: render passes failed");        return false; }
    if (!CreateBloomFramebuffers())        { SLEAK_ERROR("Bloom: framebuffers failed");         return false; }
    if (!CreateBloomDescriptorResources()) { SLEAK_ERROR("Bloom: descriptor resources failed"); return false; }
    if (!CreateBloomPipelines())           { SLEAK_ERROR("Bloom: pipelines failed");            return false; }
    if (!CreateTAAResources())             { SLEAK_ERROR("Bloom: TAA resources failed");         return false; }

    m_bloomResourcesCreated = true;
    SLEAK_INFO("Bloom + HDR scene resources created ({}x{}, {} mips)",
               scExtent.width, scExtent.height, BLOOM_MIP_COUNT);
    return true;
}

void VulkanRenderer::CleanupBloomResources() {
    if (!m_bloomResourcesCreated) return;

    if (m_bloomThresholdPipeline)   { vkDestroyPipeline(device, m_bloomThresholdPipeline, nullptr);   m_bloomThresholdPipeline = VK_NULL_HANDLE; }
    if (m_bloomDownsamplePipeline)  { vkDestroyPipeline(device, m_bloomDownsamplePipeline, nullptr);  m_bloomDownsamplePipeline = VK_NULL_HANDLE; }
    if (m_bloomUpsamplePipeline)    { vkDestroyPipeline(device, m_bloomUpsamplePipeline, nullptr);    m_bloomUpsamplePipeline = VK_NULL_HANDLE; }
    if (m_bloomCompositePipeline)   { vkDestroyPipeline(device, m_bloomCompositePipeline, nullptr);   m_bloomCompositePipeline = VK_NULL_HANDLE; }

    if (m_bloomFilterPipelineLayout)    { vkDestroyPipelineLayout(device, m_bloomFilterPipelineLayout, nullptr);    m_bloomFilterPipelineLayout = VK_NULL_HANDLE; }
    if (m_bloomCompositePipelineLayout) { vkDestroyPipelineLayout(device, m_bloomCompositePipelineLayout, nullptr); m_bloomCompositePipelineLayout = VK_NULL_HANDLE; }

    delete m_bloomThresholdShader;  m_bloomThresholdShader  = nullptr;
    delete m_bloomDownsampleShader; m_bloomDownsampleShader = nullptr;
    delete m_bloomUpsampleShader;   m_bloomUpsampleShader   = nullptr;
    delete m_bloomCompositeShader;  m_bloomCompositeShader  = nullptr;

    if (m_bloomDescriptorPool)  { vkDestroyDescriptorPool(device, m_bloomDescriptorPool, nullptr); m_bloomDescriptorPool = VK_NULL_HANDLE; }
    if (m_bloomFilterDSL)       { vkDestroyDescriptorSetLayout(device, m_bloomFilterDSL, nullptr); m_bloomFilterDSL = VK_NULL_HANDLE; }
    if (m_bloomCompositeDSL)    { vkDestroyDescriptorSetLayout(device, m_bloomCompositeDSL, nullptr); m_bloomCompositeDSL = VK_NULL_HANDLE; }

    for (auto& fb : m_bloomCompositeFramebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    m_bloomCompositeFramebuffers.clear();

    for (uint32_t m = 0; m < BLOOM_MIP_COUNT; ++m) {
        if (m_bloomMipFramebuffers[m]) { vkDestroyFramebuffer(device, m_bloomMipFramebuffers[m], nullptr); m_bloomMipFramebuffers[m] = VK_NULL_HANDLE; }
        if (m_bloomMipViews[m])        { vkDestroyImageView(device, m_bloomMipViews[m], nullptr);           m_bloomMipViews[m] = VK_NULL_HANDLE; }
    }

    if (m_bloomRenderPass)          { vkDestroyRenderPass(device, m_bloomRenderPass, nullptr);          m_bloomRenderPass = VK_NULL_HANDLE; }
    if (m_bloomAddRenderPass)       { vkDestroyRenderPass(device, m_bloomAddRenderPass, nullptr);       m_bloomAddRenderPass = VK_NULL_HANDLE; }
    if (m_bloomCompositeRenderPass) { vkDestroyRenderPass(device, m_bloomCompositeRenderPass, nullptr); m_bloomCompositeRenderPass = VK_NULL_HANDLE; }

    if (m_bloomImage)  { vkDestroyImage(device, m_bloomImage, nullptr);   m_bloomImage = VK_NULL_HANDLE; }
    if (m_bloomMemory) { vkFreeMemory(device, m_bloomMemory, nullptr);    m_bloomMemory = VK_NULL_HANDLE; }

    if (m_hdrSceneView)   { vkDestroyImageView(device, m_hdrSceneView, nullptr); m_hdrSceneView = VK_NULL_HANDLE; }
    if (m_hdrSceneImage)  { vkDestroyImage(device, m_hdrSceneImage, nullptr);    m_hdrSceneImage = VK_NULL_HANDLE; }
    if (m_hdrSceneMemory) { vkFreeMemory(device, m_hdrSceneMemory, nullptr);     m_hdrSceneMemory = VK_NULL_HANDLE; }

    if (m_bloomSampler) { vkDestroySampler(device, m_bloomSampler, nullptr); m_bloomSampler = VK_NULL_HANDLE; }

    CleanupTAAResources();
    m_bloomResourcesCreated = false;
}

// Write a filter-DSL descriptor set for a given source view.
static void WriteSingleImageSampler(VkDevice dev, VkDescriptorSet set,
                                    VkImageView view, VkSampler sampler,
                                    VkImageLayout layout) {
    VkDescriptorImageInfo ii{};
    ii.imageLayout = layout;
    ii.imageView   = view;
    ii.sampler     = sampler;

    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = set;
    w.dstBinding      = 0;
    w.dstArrayElement = 0;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo      = &ii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

void VulkanRenderer::RenderBloomPass() {
    if (!m_bloomResourcesCreated) return;

    // Bloom disabled: skip the expensive 6-mip threshold/downsample/upsample
    // chain. The composite still samples bloomMip[0] (binding 1), so leave it in
    // a defined SHADER_READ_ONLY state by clearing only that one mip to black.
    // Composite also pushes bloomStrength=0 so even a stale mip adds nothing.
    // Mirrors the SSAO/SSR disabled-fallback pattern.
    if (!m_bloomEnabled) {
        // Already primed to black SHADER_READ_ONLY — composite also pushes
        // bloomStrength=0, so skip the redundant per-frame clear. Re-prime only
        // if the enabled path dirtied it.
        if (m_bloomFallbackPrimed) return;
        VkImageMemoryBarrier toClear{};
        toClear.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toClear.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toClear.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.srcAccessMask       = 0;
        toClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toClear.image               = m_bloomImage;
        toClear.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toClear);

        VkClearColorValue black{};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, m_bloomImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

        VkImageMemoryBarrier toRead = toClear;
        toRead.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toRead);
        m_bloomFallbackPrimed = true;
        return;
    }

    // Enabled path dirties bloom mip0; force a re-prime if bloom is later
    // disabled (composite's bloomStrength=0 already neutralizes stale content,
    // but keep the buffer well-defined for consistency).
    m_bloomFallbackPrimed = false;

    // Per-frame transition set index layout:
    //   [0]              : threshold  (sceneHDR -> bloomMip0)
    //   [1..MIP-1]       : downsamples (bloomMip[i-1] -> bloomMip[i])
    //   [MIP..MIP*2-2]   : upsamples  (bloomMip[MIP-1-k] -> bloomMip[MIP-2-k])
    // BLOOM_TRANSITION_COUNT = 1 + (MIP-1) + (MIP-1) = 11 for MIP=6.
    auto& setArray = m_bloomFilterSets[currentFrame];

    // ---------- 1. Threshold / prefilter pass ----------
    //   Input: sceneHDR (already SHADER_READ_ONLY_OPTIMAL via forward RP finalLayout)
    //   Output: bloomMip[0]
    WriteSingleImageSampler(device, setArray[0], m_hdrSceneView, m_bloomSampler,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    {
        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_bloomRenderPass;
        rp.framebuffer       = m_bloomMipFramebuffers[0];
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = m_bloomMipExtents[0];
        rp.clearValueCount   = 0;
        vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
        FillFullscreenViewportScissor(command, m_bloomMipExtents[0]);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomThresholdPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_bloomFilterPipelineLayout, 0, 1, &setArray[0], 0, nullptr);
        struct { float threshold, knee, p0, p1; } thresh{ 1.0f, 0.5f, 0.0f, 0.0f };
        vkCmdPushConstants(command, m_bloomFilterPipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(thresh), &thresh);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
    }

    // ---------- 2. Downsample chain ----------
    for (uint32_t m = 1; m < BLOOM_MIP_COUNT; ++m) {
        uint32_t setIdx = m; // [1..MIP-1]
        WriteSingleImageSampler(device, setArray[setIdx], m_bloomMipViews[m - 1], m_bloomSampler,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_bloomRenderPass;
        rp.framebuffer       = m_bloomMipFramebuffers[m];
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = m_bloomMipExtents[m];
        rp.clearValueCount   = 0;
        vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
        FillFullscreenViewportScissor(command, m_bloomMipExtents[m]);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomDownsamplePipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_bloomFilterPipelineLayout, 0, 1, &setArray[setIdx], 0, nullptr);

        struct { float tsx, tsy, karis, p; } pc;
        pc.tsx   = 1.0f / float(m_bloomMipExtents[m - 1].width);
        pc.tsy   = 1.0f / float(m_bloomMipExtents[m - 1].height);
        pc.karis = (m == 1) ? 1.0f : 0.0f;   // Karis only on first downsample (firefly kill).
        pc.p     = 0.0f;
        vkCmdPushConstants(command, m_bloomFilterPipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
    }

    // ---------- 3. Upsample chain (additive blend onto next-larger mip) ----------
    for (uint32_t m = BLOOM_MIP_COUNT - 1; m > 0; --m) {
        uint32_t setIdx = BLOOM_MIP_COUNT + (BLOOM_MIP_COUNT - 1 - m); // [MIP..MIP*2-2]
        WriteSingleImageSampler(device, setArray[setIdx], m_bloomMipViews[m], m_bloomSampler,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_bloomAddRenderPass;
        rp.framebuffer       = m_bloomMipFramebuffers[m - 1];
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = m_bloomMipExtents[m - 1];
        rp.clearValueCount   = 0;
        vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
        FillFullscreenViewportScissor(command, m_bloomMipExtents[m - 1]);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomUpsamplePipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_bloomFilterPipelineLayout, 0, 1, &setArray[setIdx], 0, nullptr);

        struct { float tsx, tsy, radius, intensity; } pc;
        pc.tsx       = 1.0f / float(m_bloomMipExtents[m].width);
        pc.tsy       = 1.0f / float(m_bloomMipExtents[m].height);
        pc.radius    = 1.0f;
        pc.intensity = 1.0f;
        vkCmdPushConstants(command, m_bloomFilterPipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
    }
}

void VulkanRenderer::RenderBloomCompositePass() {
    if (!m_bloomResourcesCreated) return;

    // Bind sceneHDR (binding 0) + bloomMip[0] (binding 1) + SSR (binding 2).
    // When SSR is disabled/unavailable, fall back to the 1x1 default texture
    // which the shader reads as rgb=0 (multiplied by premultiplied alpha it
    // adds nothing) — safe to leave always bound.
    VkDescriptorSet compSet = m_bloomCompositeSets[currentFrame];
    std::array<VkDescriptorImageInfo, 3> infos{};
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[0].imageView   = m_hdrSceneView;
    infos[0].sampler     = m_bloomSampler;

    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].imageView   = m_bloomMipViews[0];
    infos[1].sampler     = m_bloomSampler;

    // RenderSSRPass() guarantees m_ssrImage ends in SHADER_READ_ONLY_OPTIMAL
    // (either via pipeline output or a cleared-to-black fallback when SSR
    // is disabled). Fall back to the bloom mip view only when SSR resources
    // haven't been created yet (should never happen once init completes).
    infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (m_ssrResourcesCreated && m_ssrView != VK_NULL_HANDLE) {
        infos[2].imageView = m_ssrView;
        infos[2].sampler   = m_ssrSampler ? m_ssrSampler : m_bloomSampler;
    } else {
        infos[2].imageView = m_bloomMipViews[0];
        infos[2].sampler   = m_bloomSampler;
    }

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = compSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &infos[i];
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = m_bloomCompositeRenderPass;
    rp.framebuffer       = m_bloomCompositeFramebuffers[CurrentFrameIndex];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = scExtent;
    rp.clearValueCount   = 0;

    vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    FillFullscreenViewportScissor(command, scExtent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomCompositePipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_bloomCompositePipelineLayout, 0, 1, &compSet, 0, nullptr);

    struct { float bloomStrength, exposure, p0, p1; } pc;
    // bloom disabled -> 0 so the (black) mip contributes nothing
    pc.bloomStrength = m_bloomEnabled ? 0.06f : 0.0f;   // UE4-style soft bloom
    pc.exposure      = m_exposure;
    pc.p0            = 0.0f;
    pc.p1            = 0.0f;
    vkCmdPushConstants(command, m_bloomCompositePipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(command, 3, 1, 0, 0);

    // ImGui draws inside the composite pass, AFTER the HDR → LDR tonemap.
    if (bImFrameActive) {
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
    }

    vkCmdEndRenderPass(command);
}

}
}
