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

}
}
