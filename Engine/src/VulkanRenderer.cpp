#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanBuffer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanTexture.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanCubemapTexture.hpp"
#include "../../include/private/Graphics/RenderCommandQueue.hpp"
#include <Runtime/MeshData.hpp>

#include <SDL3/SDL_vulkan.h>
#include <Window.hpp>
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
#include "Graphics/Vertex.hpp"
#include "Graphics/ResourceManager.hpp"
#include "Logger.hpp"
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
        shadowPassInfo.renderArea.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
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
        shadowViewport.width = static_cast<float>(SHADOW_MAP_SIZE);
        shadowViewport.height = static_cast<float>(SHADOW_MAP_SIZE);
        shadowViewport.minDepth = 0.0f;
        shadowViewport.maxDepth = 1.0f;
        vkCmdSetViewport(command, 0, 1, &shadowViewport);

        VkRect2D shadowScissor{};
        shadowScissor.offset = {0, 0};
        shadowScissor.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
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

void VulkanRenderer::BeginSkyboxPass() {
    if (!bFrameStarted) return;
    // Skybox only makes sense in forward context — not inside the GBuffer geometry pass
    // where set 0 is a PBR material descriptor set incompatible with pipelineLay.
    if (m_inGeometryPass) return;
    if (skyboxPipeline == VK_NULL_HANDLE || !m_skyboxDescriptorsWritten)
        return;

    m_inVoxelPass = false;  // prevent BindVertexBuffer from overriding this pipeline
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      skyboxPipeline);

    if (CurrentFrameIndex < skyboxDescriptorSets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &skyboxDescriptorSets[CurrentFrameIndex], 0, nullptr);
    }
}

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

bool VulkanRenderer::InitVulkan() {
    try {
        instance = VK_NULL_HANDLE;

        std::vector<const char*> requiredExtensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME
        };

        #ifdef PLATFORM_LINUX
        {
            // Only request surface extensions actually available
            // (blindly requesting both breaks capture tools like RenderDoc)
            uint32_t surfExtCount = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &surfExtCount, nullptr);
            std::vector<VkExtensionProperties> surfExts(surfExtCount);
            vkEnumerateInstanceExtensionProperties(nullptr, &surfExtCount, surfExts.data());
            auto hasSurfExt = [&](const char* name) {
                for (auto& e : surfExts)
                    if (strcmp(e.extensionName, name) == 0) return true;
                return false;
            };
            if (hasSurfExt("VK_KHR_wayland_surface"))
                requiredExtensions.push_back("VK_KHR_wayland_surface");
            if (hasSurfExt("VK_KHR_xlib_surface"))
                requiredExtensions.push_back("VK_KHR_xlib_surface");
        }
        #elif defined(PLATFORM_WIN)
            requiredExtensions.push_back(
                VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        #elif defined(TARGET_OS_MAC) || defined(TARGET_OS_IOS)
            requiredExtensions.push_back(VK_MVK_MOLTENVK_EXTENSION_NAME);
        #endif

        // Always attempt to enable validation layers so GPU errors are
        // reported via the debug messenger as SLEAK_ERROR messages rather
        // than silent VK_ERROR_DEVICE_LOST crashes.  If the layer is not
        // installed the instance still creates successfully (empty list).
        std::vector<const char*> enabledLayers;
        {
            uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            std::vector<VkLayerProperties> availableLayers(layerCount);
            vkEnumerateInstanceLayerProperties(&layerCount,
                                                availableLayers.data());

            const char* desiredLayer = "VK_LAYER_KHRONOS_validation";
            for (const auto& layer : availableLayers) {
                if (strcmp(layer.layerName, desiredLayer) == 0) {
                    enabledLayers.push_back(desiredLayer);
                    SLEAK_INFO("Vulkan validation layer enabled");
                    break;
                }
            }
            if (enabledLayers.empty()) {
                SLEAK_WARN("VK_LAYER_KHRONOS_validation not available — GPU errors will not be reported");
            }
        }
        if (enabledLayers.empty()) {
            auto it = std::find_if(
                requiredExtensions.begin(), requiredExtensions.end(),
                [](const char* ext) {
                    return strcmp(ext,
                                  VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
                });
            if (it != requiredExtensions.end()) {
                requiredExtensions.erase(it);
            }
        }

        // Check and list vulkan extensions
        uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
                                                nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
                                                extensions.data());

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "SleakEngine";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "Sleak Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount =
            static_cast<uint32_t>(requiredExtensions.size());
        createInfo.ppEnabledExtensionNames = requiredExtensions.data();
        createInfo.enabledLayerCount =
            static_cast<uint32_t>(enabledLayers.size());
        createInfo.ppEnabledLayerNames = enabledLayers.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (!enabledLayers.empty()) {
            PopulateDebugMessengerCreateInfo(debugCreateInfo);
            createInfo.pNext = &debugCreateInfo;
        }

        VkResult result =
            vkCreateInstance(&createInfo, nullptr, &instance);
        if (result != VK_SUCCESS) {
            SLEAK_ERROR("Failed to create Vulkan instance!");
            return false;
        }

        SLEAK_INFO("Vulkan instance created successfully.");
        return true;

    } catch (const std::exception& e) {
        SLEAK_ERROR("Exception in InitVulkan: {}", e.what());
        return false;
    }
}

bool VulkanRenderer::CreateDevice() {
    // Enumerate physical devices
    uint32_t deviceCount = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &deviceCount,
                                                  nullptr);
    if (result != VK_SUCCESS || deviceCount == 0)
        SLEAK_RETURN_ERR("No device found in computer!")

    GPUs.resize(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, GPUs.data());

    SLEAK_INFO("Found totally {} devices in computer", deviceCount);

    // Pick best GPU (prefer discrete)
    physicalDevice = GPUs[0];
    for (auto& dev : GPUs) {
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceMemoryProperties memprops;
        vkGetPhysicalDeviceProperties(dev, &props);
        vkGetPhysicalDeviceMemoryProperties(dev, &memprops);

        std::string type;
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                type = "Integrated GPU"; break;
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                type = "Discrete GPU"; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                type = "Virtual GPU"; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                type = "CPU"; break;
            default:
                type = "Other";
        }

        VkDeviceSize totalDedicatedMemory = 0;
        for (uint32_t i = 0; i < memprops.memoryHeapCount; i++) {
            if (memprops.memoryHeaps[i].flags &
                VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                totalDedicatedMemory += memprops.memoryHeaps[i].size;
            }
        }
        float totalDedicatedMemoryGB =
            static_cast<float>(totalDedicatedMemory) /
            (1024.0f * 1024.0f * 1024.0f);

        SLEAK_INFO("Name: {0} Type: {1} memory: {2}",
                    props.deviceName, type, totalDedicatedMemoryGB);

        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            physicalDevice = dev;
    }

    // Make physical device available for VRAM tracking
    VulkanBuffer::SetPhysicalDevice(physicalDevice);

    // Find queue families
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
                                              nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
                                              queueFamilies.data());

    QueueIDs.GraphicsIndex = UINT32_MAX;
    QueueIDs.ComputeIndex = UINT32_MAX;
    QueueIDs.TransferIndex = UINT32_MAX;
    QueueIDs.PresentIndex = UINT32_MAX;

    for (uint32_t i = 0; i < familyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT &&
            QueueIDs.GraphicsIndex == UINT32_MAX) {
            QueueIDs.GraphicsIndex = i;
        }
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT &&
            QueueIDs.ComputeIndex == UINT32_MAX) {
            QueueIDs.ComputeIndex = i;
        }
        if (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT &&
            QueueIDs.TransferIndex == UINT32_MAX) {
            QueueIDs.TransferIndex = i;
        }

        if (QueueIDs.PresentIndex == UINT32_MAX) {
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i,
                                                  surface,
                                                  &presentSupport);
            if (presentSupport) {
                QueueIDs.PresentIndex = i;
            }
        }
    }

    if (QueueIDs.GraphicsIndex == UINT32_MAX ||
        QueueIDs.PresentIndex == UINT32_MAX)
        SLEAK_RETURN_ERR("Failed to find required queue families!")

    // If compute/transfer not found, fall back to graphics
    if (QueueIDs.ComputeIndex == UINT32_MAX)
        QueueIDs.ComputeIndex = QueueIDs.GraphicsIndex;
    if (QueueIDs.TransferIndex == UINT32_MAX)
        QueueIDs.TransferIndex = QueueIDs.GraphicsIndex;

    #ifdef _DEBUG
        SLEAK_INFO(
            "Graphics: {0} Compute: {1} Transfer: {2} Present: {3}",
            QueueIDs.GraphicsIndex, QueueIDs.ComputeIndex,
            QueueIDs.TransferIndex, QueueIDs.PresentIndex);
    #endif

    // Build unique queue create infos (no duplicates!)
    auto queueCreateInfos = GetUniqueQueueCreateInfos();

    // Get supported features
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physicalDevice, &features);

    // Query max MSAA sample count
    VkPhysicalDeviceProperties deviceProperties;
    vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
    VkSampleCountFlags counts = deviceProperties.limits.framebufferColorSampleCounts
                              & deviceProperties.limits.framebufferDepthSampleCounts;
    m_maxMsaaSampleCount = 1;
    if (counts & VK_SAMPLE_COUNT_8_BIT)  m_maxMsaaSampleCount = 8;
    else if (counts & VK_SAMPLE_COUNT_4_BIT)  m_maxMsaaSampleCount = 4;
    else if (counts & VK_SAMPLE_COUNT_2_BIT)  m_maxMsaaSampleCount = 2;
    SLEAK_INFO("Max MSAA sample count: {}", m_maxMsaaSampleCount);

    std::vector<const char*> requiredExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // Create logical device
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());
    deviceInfo.ppEnabledExtensionNames = requiredExtensions.data();
    deviceInfo.enabledExtensionCount =
        static_cast<uint32_t>(requiredExtensions.size());
    deviceInfo.pEnabledFeatures = &features;
    deviceInfo.enabledLayerCount = 0;

    result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);

    if (result != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create a logical device!")

    // Retrieve queues
    vkGetDeviceQueue(device, QueueIDs.GraphicsIndex, 0, &graphicsQueue);
    vkGetDeviceQueue(device, QueueIDs.ComputeIndex, 0, &computeQueue);
    vkGetDeviceQueue(device, QueueIDs.TransferIndex, 0, &transferQueue);
    vkGetDeviceQueue(device, QueueIDs.PresentIndex, 0, &presentQueue);

    // VMA allocator — backs all VulkanBuffer allocations.
    VulkanBuffer::InitAllocator(instance, physicalDevice, device);

    return true;
}

// Build one VkDeviceQueueCreateInfo per *unique* family index
std::vector<VkDeviceQueueCreateInfo>
VulkanRenderer::GetUniqueQueueCreateInfos() {
    std::set<uint32_t> uniqueFamilies = {
        QueueIDs.GraphicsIndex, QueueIDs.ComputeIndex,
        QueueIDs.TransferIndex, QueueIDs.PresentIndex};

    std::vector<VkDeviceQueueCreateInfo> result;

    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &QueueIDs.GraphicsPriority;
        result.push_back(info);
    }

    return result;
}

bool VulkanRenderer::SetupDebugMessenger() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    PopulateDebugMessengerCreateInfo(createInfo);

    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");

    if (func) {
        VkResult result =
            func(instance, &createInfo, nullptr, &debugMessenger);
        if (result != VK_SUCCESS)
            SLEAK_RETURN_ERR("Failed to setup Debug Messenger!")

        vkDestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!vkDestroyDebugUtilsMessengerEXT)
            SLEAK_ERROR(
                "Failed to setup Debug Messenger Destroy Function!")
    } else {
        SLEAK_ERROR(
            "vkCreateDebugUtilsMessengerEXT could not found!")
    }

    return true;
}

bool VulkanRenderer::CreateSurface() {
    SDL_Vulkan_LoadLibrary(NULL);
    bool result = SDL_Vulkan_CreateSurface(sdlWindow->GetSDLWindow(),
                                            instance, nullptr, &surface);

    if (!result || surface == VK_NULL_HANDLE) {
        const char* error = SDL_GetError();
        SLEAK_ERROR("Caught an SDL error! {}", error);
        SLEAK_RETURN_ERR("Failed to create a render surface for Vulkan!");
    }

    return true;
}

bool VulkanRenderer::CreateSwapChain() {
    auto details = QuerySwapchain();
    if (!details.has_value())
        return false;

    if (details->formats.empty() && details->presentModes.empty())
        SLEAK_RETURN_ERR("Swapchain is not supported for this GPU!");

    auto format = ChooseFormat(details->formats);
    auto mode = ChoosePresentMode(details->presentModes);
    auto extent = ChooseExtend(details.value());

    uint32_t imageCount = details->caps.minImageCount + 1;
    if (details->caps.maxImageCount > 0 &&
        imageCount > details->caps.maxImageCount)
        imageCount = details->caps.maxImageCount;

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.minImageCount = imageCount;
    info.imageColorSpace = format.colorSpace;
    info.imageFormat = format.format;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.presentMode = mode;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t indices[] = {QueueIDs.GraphicsIndex, QueueIDs.PresentIndex};

    if (QueueIDs.GraphicsIndex != QueueIDs.PresentIndex) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = indices;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    info.surface = surface;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.preTransform = details->caps.currentTransform;
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    VkResult result =
        vkCreateSwapchainKHR(device, &info, nullptr, &swapChain);
    if (result != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create swap chain for renderer!")

    uint32_t scImageCount = 0;
    vkGetSwapchainImagesKHR(device, swapChain, &scImageCount, nullptr);
    swapChainImages.resize(scImageCount);
    vkGetSwapchainImagesKHR(device, swapChain, &scImageCount,
                             swapChainImages.data());

    scImageFormat = format.format;
    scExtent = extent;

    return true;
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

void VulkanRenderer::CleanupSwapChain() {
    CleanupDepthResources();
    CleanupMSAAColorResources();

    for (auto framebuffer : swapChainFramebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    swapChainFramebuffers.clear();

    for (auto imageView : swapChainImageViews) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    swapChainImageViews.clear();

    if (swapChain) {
        vkDestroySwapchainKHR(device, swapChain, nullptr);
        swapChain = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::CreateMSAAColorResources() {
    if (m_msaaSamples == VK_SAMPLE_COUNT_1_BIT)
        return true; // No MSAA image needed

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = scExtent.width;
    imageInfo.extent.height = scExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = scImageFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.samples = m_msaaSamples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &m_msaaColorImage) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create MSAA color image!");

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, m_msaaColorImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &m_msaaColorImageMemory) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to allocate MSAA color image memory!");

    vkBindImageMemory(device, m_msaaColorImage, m_msaaColorImageMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_msaaColorImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = scImageFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_msaaColorImageView) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create MSAA color image view!");

    return true;
}

void VulkanRenderer::CleanupMSAAColorResources() {
    if (m_msaaColorImageView) {
        vkDestroyImageView(device, m_msaaColorImageView, nullptr);
        m_msaaColorImageView = VK_NULL_HANDLE;
    }
    if (m_msaaColorImage) {
        vkDestroyImage(device, m_msaaColorImage, nullptr);
        m_msaaColorImage = VK_NULL_HANDLE;
    }
    if (m_msaaColorImageMemory) {
        vkFreeMemory(device, m_msaaColorImageMemory, nullptr);
        m_msaaColorImageMemory = VK_NULL_HANDLE;
    }
}

VkSampleCountFlagBits VulkanRenderer::GetMaxUsableSampleCount() {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts
                              & props.limits.framebufferDepthSampleCounts;
    if (counts & VK_SAMPLE_COUNT_8_BIT) return VK_SAMPLE_COUNT_8_BIT;
    if (counts & VK_SAMPLE_COUNT_4_BIT) return VK_SAMPLE_COUNT_4_BIT;
    if (counts & VK_SAMPLE_COUNT_2_BIT) return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
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

bool VulkanRenderer::CreateImageViews() {
    swapChainImageViews.resize(swapChainImages.size());

    for (size_t i = 0; i < swapChainImageViews.size(); i++) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = swapChainImages[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = scImageFormat;

        info.components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY};

        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = 0;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = 1;

        auto result = vkCreateImageView(device, &info, nullptr,
                                         &swapChainImageViews[i]);
        if (result != VK_SUCCESS)
            SLEAK_RETURN_ERR(
                "Failed to create image view of swapchain, index: {}", i);
    }

    return true;
}

bool VulkanRenderer::CreateDepthResources() {
    depthFormat = FindDepthFormat();

    // Create depth image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = scExtent.width;
    imageInfo.extent.height = scExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = depthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Always add SAMPLED_BIT so deferred lighting pass can read depth.
    // When deferred is enabled, force 1x samples (GBuffer is non-MSAA).
    VkSampleCountFlagBits depthSamples = m_deferredEnabled
                                        ? VK_SAMPLE_COUNT_1_BIT : m_msaaSamples;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                    | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = depthSamples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &depthImage) !=
        VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create depth image!");

    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, depthImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &depthImageMemory) !=
        VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to allocate depth image memory!");

    vkBindImageMemory(device, depthImage, depthImageMemory, 0);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &depthImageView) !=
        VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create depth image view!");

    return true;
}

void VulkanRenderer::CleanupDepthResources() {
    if (depthImageView) {
        vkDestroyImageView(device, depthImageView, nullptr);
        depthImageView = VK_NULL_HANDLE;
    }
    if (depthImage) {
        vkDestroyImage(device, depthImage, nullptr);
        depthImage = VK_NULL_HANDLE;
    }
    if (depthImageMemory) {
        vkFreeMemory(device, depthImageMemory, nullptr);
        depthImageMemory = VK_NULL_HANDLE;
    }
}

VkFormat VulkanRenderer::FindDepthFormat() {
    std::vector<VkFormat> candidates = {VK_FORMAT_D32_SFLOAT,
                                         VK_FORMAT_D32_SFLOAT_S8_UINT,
                                         VK_FORMAT_D24_UNORM_S8_UINT};

    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

        if (props.optimalTilingFeatures &
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return format;
        }
    }

    return VK_FORMAT_D32_SFLOAT;  // fallback
}

uint32_t VulkanRenderer::FindMemoryType(
    uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) ==
                properties) {
            return i;
        }
    }

    SLEAK_ERROR("Failed to find suitable memory type!");
    return 0;
}

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
    boneUBOBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
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
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = totalSets;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr,
                                &descriptorPool) != VK_SUCCESS)
        SLEAK_RETURN_ERR("Failed to create descriptor pool!");

    return true;
}

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

    texture->SetDescriptorSets(std::move(sets));
}

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

std::optional<SwapchainDetails> VulkanRenderer::QuerySwapchain() {
    SwapchainDetails details;

    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice, surface, &details.caps);
    if (result != VK_SUCCESS) {
        SLEAK_ERROR("Failed to retrieve surface information!");
        return {};
    }

    // Fixed: use resize() not reserve()
    uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface,
                                                   &formatCount, nullptr);
    if (result != VK_SUCCESS || formatCount < 1) {
        SLEAK_ERROR("Failed to retrieve supported surface formats");
        return {};
    }

    details.formats.resize(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface,
                                          &formatCount,
                                          details.formats.data());

    // Get surface present modes
    uint32_t modeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physicalDevice, surface, &modeCount, nullptr);
    if (result != VK_SUCCESS || modeCount < 1) {
        SLEAK_ERROR("Failed to retrieve present modes!");
        return {};
    }

    details.presentModes.resize(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                               &modeCount,
                                               details.presentModes.data());

    return details;
}

VkSurfaceFormatKHR VulkanRenderer::ChooseFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) {
    // Prefer UNORM so the GPU does NOT apply automatic sRGB gamma encoding
    // on output. The game renders in sRGB/gamma space already (no linear
    // pipeline), so using _SRGB would gamma-encode everything twice —
    // producing a washed-out, overbright image. _UNORM writes values as-is,
    // matching the behaviour of DX11/DX12 DXGI_FORMAT_*_UNORM swap chains.
    for (auto& format : formats)
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;

    // Second preference: R8G8B8A8_UNORM
    for (auto& format : formats)
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;

    return formats[0];
}

VkPresentModeKHR VulkanRenderer::ChoosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) {
    if (m_vsync) {
        // VSync ON: FIFO is guaranteed and provides VSync
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // VSync OFF: prefer MAILBOX (no tearing, uncapped), then IMMEDIATE
    for (auto& mode : modes)
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            return mode;
    for (auto& mode : modes)
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            return mode;

    return VK_PRESENT_MODE_FIFO_KHR;
}

// Fixed: clamp height using height, not width
VkExtent2D VulkanRenderer::ChooseExtend(SwapchainDetails details) {
    if (details.caps.currentExtent.width !=
        std::numeric_limits<uint32_t>::max())
        return details.caps.currentExtent;

    int width, height;
    SDL_GetWindowSizeInPixels(sdlWindow->GetSDLWindow(), &width, &height);

    VkExtent2D actualExtent = {static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height)};

    actualExtent.width =
        std::clamp(actualExtent.width,
                   details.caps.minImageExtent.width,
                   details.caps.maxImageExtent.width);

    actualExtent.height =
        std::clamp(actualExtent.height,
                   details.caps.minImageExtent.height,
                   details.caps.maxImageExtent.height);

    return actualExtent;
}

void VulkanRenderer::PopulateDebugMessengerCreateInfo(
    VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
    createInfo = {};
    createInfo.sType =
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = &VulkanRenderer::Validation;
}

VkBool32 VulkanRenderer::Validation(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {
    std::string out = std::format("Vulkan: {} \n Type: {}",
                                   pCallbackData->pMessage, messageTypes);

    switch (messageSeverity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            SLEAK_WARN(out);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            SLEAK_ERROR(out);
            break;
        default:
            SLEAK_INFO(out);
    }

    return VK_FALSE;
}

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

    // Create a dedicated descriptor pool for ImGUI (extra sets for user textures)
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 64;
    poolInfo.poolSizeCount = 1;
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
    rasterizer.cullMode = VK_CULL_MODE_NONE;
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
    } else {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_voxelPipeline);
    }
}

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

void VulkanRenderer::BeginDebugLinePass() {
    if (!bFrameStarted) return;
    if (debugLinePipeline == VK_NULL_HANDLE) {
        if (!CreateDebugLinePipeline()) return;
    }
    m_inVoxelPass = false;  // prevent BindVertexBuffer from overriding this pipeline
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      debugLinePipeline);
}

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

bool VulkanRenderer::CreateBoneUBOResources() {
    if (m_boneUBOCreated) return true;

    static constexpr uint32_t MAX_BONES = 256;
    static constexpr VkDeviceSize boneUBOSize = MAX_BONES * 64; // 256 mat4 = 16384 bytes

    // Create per-frame UBO buffers (host-visible, coherent for fast CPU writes)
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = boneUBOSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &bufferInfo, nullptr, &boneUBOBuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("Failed to create bone UBO buffer!");
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, boneUBOBuffers[i], &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &boneUBOMemory[i]) != VK_SUCCESS) {
            SLEAK_ERROR("Failed to allocate bone UBO memory!");
            return false;
        }

        vkBindBufferMemory(device, boneUBOBuffers[i], boneUBOMemory[i], 0);
        vkMapMemory(device, boneUBOMemory[i], 0, boneUBOSize, 0, &boneUBOMapped[i]);

        // Initialize with identity matrices
        auto* matrices = static_cast<float*>(boneUBOMapped[i]);
        for (uint32_t b = 0; b < MAX_BONES; ++b) {
            // Identity matrix in column-major order
            for (int c = 0; c < 16; ++c)
                matrices[b * 16 + c] = (c % 5 == 0) ? 1.0f : 0.0f;
        }
    }

    // Create descriptor pool for bone UBO
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &boneDescriptorPool) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create bone descriptor pool!");
        return false;
    }

    // Allocate descriptor sets
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(boneDescriptorSetLayout);

    VkDescriptorSetAllocateInfo dsAllocInfo{};
    dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool = boneDescriptorPool;
    dsAllocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    dsAllocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(device, &dsAllocInfo, boneDescriptorSets.data()) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate bone descriptor sets!");
        return false;
    }

    // Write descriptor sets pointing to UBO buffers
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = boneUBOBuffers[i];
        bufInfo.offset = 0;
        bufInfo.range = boneUBOSize;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = boneDescriptorSets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    m_boneUBOCreated = true;
    SLEAK_INFO("VulkanRenderer: Bone UBO resources created ({} bytes per frame)", boneUBOSize);
    return true;
}

void VulkanRenderer::CleanupBoneUBOResources() {
    if (!m_boneUBOCreated) return;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (boneUBOMapped[i]) {
            vkUnmapMemory(device, boneUBOMemory[i]);
            boneUBOMapped[i] = nullptr;
        }
        if (boneUBOBuffers[i]) {
            vkDestroyBuffer(device, boneUBOBuffers[i], nullptr);
            boneUBOBuffers[i] = VK_NULL_HANDLE;
        }
        if (boneUBOMemory[i]) {
            vkFreeMemory(device, boneUBOMemory[i], nullptr);
            boneUBOMemory[i] = VK_NULL_HANDLE;
        }
    }
    if (boneDescriptorPool) {
        vkDestroyDescriptorPool(device, boneDescriptorPool, nullptr);
        boneDescriptorPool = VK_NULL_HANDLE;
    }
    m_boneUBOCreated = false;
}

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
    static constexpr uint32_t MAX_BONE_UBO_SIZE = 256 * 64; // MAX_BONES * sizeof(mat4)
    if (size > MAX_BONE_UBO_SIZE) size = MAX_BONE_UBO_SIZE;

    // Copy bone matrices to mapped UBO (use currentFrame, not CurrentFrameIndex
    // which is the swapchain image index and can exceed MAX_FRAMES_IN_FLIGHT)
    memcpy(boneUBOMapped[currentFrame], data, size);

    // Bind bone descriptor set at set index 1.
    // In the GBuffer geometry pass the active layout is m_gbufferGeomLayout;
    // outside it pipelineLay is active. The layout used here must match the
    // pipeline that will draw, because Vulkan invalidates sets when layouts
    // are incompatible at lower-numbered sets (set 0 differs between the two).
    VkPipelineLayout boneBindLayout = (m_inGeometryPass && m_gbufferGeomLayout != VK_NULL_HANDLE)
                                      ? m_gbufferGeomLayout : pipelineLay;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            boneBindLayout, 1, 1,
                            &boneDescriptorSets[currentFrame],
                            0, nullptr);
}

bool VulkanRenderer::CreateShadowResources() {
    // 1. Create shadow depth image (2048x2048, D32_SFLOAT)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &m_shadowImage) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow map image!");
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, m_shadowImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &m_shadowImageMemory) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to allocate shadow map memory!");
        return false;
    }

    vkBindImageMemory(device, m_shadowImage, m_shadowImageMemory, 0);

    // 2. Create image view (DEPTH aspect)
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

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_shadowImageView) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow map image view!");
        return false;
    }

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

    // 5. Create framebuffer
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_shadowRenderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_shadowImageView;
    fbInfo.width = SHADOW_MAP_SIZE;
    fbInfo.height = SHADOW_MAP_SIZE;
    fbInfo.layers = 1;

    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_shadowFramebuffer) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to create shadow framebuffer!");
        return false;
    }

    // 6. Transition shadow image to DEPTH_STENCIL_READ_ONLY_OPTIMAL so the
    //    descriptor is valid even before the first shadow pass runs.
    //    Depth images must use this layout (not SHADER_READ_ONLY_OPTIMAL)
    //    for sampler access; the wrong layout causes VK_ERROR_DEVICE_LOST.
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

        vkCmdPipelineBarrier(cmdBuf,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

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

    // 7. Create shadow pipeline
    if (!CreateShadowPipeline()) {
        SLEAK_ERROR("Failed to create shadow pipeline!");
        return false;
    }

    // Write shadow sampler to set 3 descriptors (UBO resources already created).
    // Binding 0 uses the compare sampler for hardware PCF; binding 1 uses the
    // raw sampler so the PCSS blocker search can read un-compared depth values.
    if (m_lightUBOCreated && m_shadowImageView && m_shadowSampler && m_shadowRawSampler) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            // Depth images must use DEPTH_STENCIL_READ_ONLY_OPTIMAL for sampler access.
            VkDescriptorImageInfo compareInfo{};
            compareInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            compareInfo.imageView = m_shadowImageView;
            compareInfo.sampler = m_shadowSampler;

            VkDescriptorImageInfo rawInfo{};
            rawInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            rawInfo.imageView = m_shadowImageView;
            rawInfo.sampler = m_shadowRawSampler;

            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_shadowSamplerDescriptorSets[i];
            writes[0].dstBinding = 0;
            writes[0].dstArrayElement = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &compareInfo;

            writes[1] = writes[0];
            writes[1].dstBinding = 1;
            writes[1].pImageInfo = &rawInfo;

            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }

    m_shadowResourcesCreated = true;
    SLEAK_INFO("VulkanRenderer: Shadow mapping resources created ({}x{} shadow map)",
               SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    return true;
}

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

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

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
    rasterizer.cullMode = VK_CULL_MODE_NONE; // No culling in shadow pass — avoids winding issues
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

    // Create descriptor pool for light UBO + shadow samplers.
    // Two shadow samplers per frame now (compare + raw for PCSS blocker search).
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * 2;

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

    // Write default shadow sampler descriptors (using default texture as placeholder)
    // These will be overwritten with actual shadow map when shadow resources are created.
    // Both binding=0 (compare) and binding=1 (raw) must be populated or the layout
    // is incomplete and first-frame sampling reads undefined memory.
    if (m_defaultTexture) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = m_defaultTexture->GetImageView();
            imageInfo.sampler = m_defaultTexture->GetSampler();

            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_shadowSamplerDescriptorSets[i];
            writes[0].dstBinding = 0;
            writes[0].dstArrayElement = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &imageInfo;

            writes[1] = writes[0];
            writes[1].dstBinding = 1;

            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }

    m_lightUBOCreated = true;
    SLEAK_INFO("VulkanRenderer: Light UBO and shadow sampler resources created");
    return true;
}

void VulkanRenderer::CleanupShadowResources() {
    if (m_shadowPipeline) {
        vkDestroyPipeline(device, m_shadowPipeline, nullptr);
        m_shadowPipeline = VK_NULL_HANDLE;
    }
    delete m_shadowShader;
    m_shadowShader = nullptr;

    if (m_shadowFramebuffer) {
        vkDestroyFramebuffer(device, m_shadowFramebuffer, nullptr);
        m_shadowFramebuffer = VK_NULL_HANDLE;
    }
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
    if (m_shadowImageView) {
        vkDestroyImageView(device, m_shadowImageView, nullptr);
        m_shadowImageView = VK_NULL_HANDLE;
    }
    if (m_shadowImage) {
        vkDestroyImage(device, m_shadowImage, nullptr);
        m_shadowImage = VK_NULL_HANDLE;
    }
    if (m_shadowImageMemory) {
        vkFreeMemory(device, m_shadowImageMemory, nullptr);
        m_shadowImageMemory = VK_NULL_HANDLE;
    }

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

void VulkanRenderer::BeginShadowPass() {
    m_shadowPassActive = true;
    m_shadowPCCacheValid = false;
}

void VulkanRenderer::EndShadowPass() {
    m_shadowPassActive = false;
}

void VulkanRenderer::UpdateShadowLightUBO(const void* data, uint32_t size) {
    if (!m_lightUBOCreated || !data) return;
    uint32_t copySize = std::min(size, static_cast<uint32_t>(sizeof(ShadowLightUBO)));
    memcpy(m_lightUBOMapped[currentFrame], data, copySize);
}

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

// ============================================================
// Deferred Rendering — GBuffer format table
// ============================================================
const VkFormat VulkanRenderer::m_gbufferFormats[VulkanRenderer::GBUFFER_COUNT] = {
    VK_FORMAT_R8G8B8A8_UNORM,        // RT0: AlbedoAO
    VK_FORMAT_R16G16B16A16_SFLOAT,   // RT1: NormalRough
    VK_FORMAT_R16G16B16A16_SFLOAT,   // RT2: MetalEmit (HDR emissive needs float)
    VK_FORMAT_R32G32B32A32_SFLOAT,   // RT3: WorldPos
};

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

    // Now that SSAO inputs (gNormalRough, gWorldPos, gDepth) and SSAO blur
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

// ============================================================
// CreateGBufferRenderPass
// 4 attachments: RT0, RT1, RT2, Depth
// ============================================================
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

// ============================================================
// CreateGBufferFramebuffer
// ============================================================
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

// ============================================================
// CreateGBufferPipeline — reuses pipelineLay (same sets 0-3)
// ============================================================
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

// ============================================================
// CreateSkinnedGbufferPipeline
// Same as CreateGBufferPipeline but uses skinned_shader.vert so that
// skinned meshes write into the GBuffer (4 color attachments) correctly.
// ============================================================
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

// ============================================================
// CreateLightingRenderPass
// 1 color attachment, no depth, loadOp=DONT_CARE → COLOR_ATTACHMENT_OPTIMAL
// ============================================================
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

// ============================================================
// CreateLightingFramebuffers — single HDR scene target (post-processing
// pipeline writes to HDR scene, then composite pass tonemaps into swapchain).
// We still use a vector so the rest of the code can index by CurrentFrameIndex,
// but every entry points at the same HDR framebuffer.
// ============================================================
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

// ============================================================
// CreateLightingPipeline — fullscreen triangle, no vertex input
// Layout: sets 0 (gbuffer samplers), 1 (deferredCB), 2 (lightUBO), 3 (shadowSampler)
// ============================================================
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

// ============================================================
// CreateForwardRenderPass
// Color LOAD → STORE, Depth LOAD → DONT_CARE
// Writes to the HDR scene image (same as the lighting pass).  Ends in
// SHADER_READ_ONLY_OPTIMAL so the bloom chain can sample the composed HDR
// scene without an extra manual barrier.
// ============================================================
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

// ============================================================
// CreateForwardFramebuffers
// Writes to the HDR scene image (same as lighting pass) + shared depth.
// One framebuffer per swapchain image for CurrentFrameIndex indexing, but
// each entry points at the same HDR target.
// ============================================================
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

// ============================================================
// CreateGBufferDescriptorSets
// m_gbufferSamplerDSL: 6 combined image samplers for lighting pass set 0
//   binding 0..3 = GBuffer color RTs (RT0=AlbedoAO, RT1=NormalRough,
//   RT2=MetalEmit, RT3=WorldPos), binding 4 = depth, binding 5 = shadow.
// ============================================================
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
    // binding 0: RT0, 1: RT1, 2: RT2, 3: RT3 (WorldPos), 4: depth,
    // binding 5: shadow compare sampler (hardware PCF),
    // binding 6: shadow raw sampler    (PCSS blocker search)
    // binding 7: screen-space AO       (R8, bilateral blurred)
    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
    for (uint32_t b = 0; b < 8; ++b) {
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

    // Pool: 8 samplers × MAX_FRAMES_IN_FLIGHT sets
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 8 * MAX_FRAMES_IN_FLIGHT;

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

// ============================================================
// UpdateGBufferDescriptors — called just before lighting pass
// ============================================================
void VulkanRenderer::UpdateGBufferDescriptors() {
    // Only update the descriptor set for the current frame slot.
    // BeginRender() already waited on this frame's fence, so its
    // descriptor set is safe to update.  Updating other slots would
    // race with the GPU still consuming them.
    uint32_t f = currentFrame;
    std::array<VkDescriptorImageInfo, 8> imageInfos{};

    // RT0..RT3 (AlbedoAO, NormalRough, MetalEmit, WorldPos)
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[i].imageView   = m_gbufferViews[i];
        imageInfos[i].sampler     = m_gbufferSampler;
    }

    // Depth (binding 4)
    imageInfos[4].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    imageInfos[4].imageView   = depthImageView;
    imageInfos[4].sampler     = m_depthSampler;

    // Shadow map (binding 5) — compare sampler for hardware PCF.
    // Depth images must use DEPTH_STENCIL_READ_ONLY_OPTIMAL (not SHADER_READ_ONLY_OPTIMAL)
    // when accessed as a sampler; using the wrong layout causes VK_ERROR_DEVICE_LOST.
    // Fall back to the default 1x1 white texture when no shadow map is available.
    if (m_shadowImageView) {
        imageInfos[5].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        imageInfos[5].imageView   = m_shadowImageView;
        imageInfos[5].sampler     = m_shadowSampler ? m_shadowSampler
                                                     : (m_defaultTexture ? m_defaultTexture->GetSampler() : VK_NULL_HANDLE);
    } else if (m_defaultTexture) {
        imageInfos[5].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[5].imageView   = m_defaultTexture->GetImageView();
        imageInfos[5].sampler     = m_defaultTexture->GetSampler();
    }

    // Shadow map raw (binding 6) — non-compare sampler for PCSS blocker search
    if (m_shadowImageView) {
        imageInfos[6].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        imageInfos[6].imageView   = m_shadowImageView;
        imageInfos[6].sampler     = m_shadowRawSampler ? m_shadowRawSampler
                                                        : (m_defaultTexture ? m_defaultTexture->GetSampler() : VK_NULL_HANDLE);
    } else if (m_defaultTexture) {
        imageInfos[6].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[6].imageView   = m_defaultTexture->GetImageView();
        imageInfos[6].sampler     = m_defaultTexture->GetSampler();
    }

    // SSAO (binding 7) — fallback to default white texture when SSAO isn't ready,
    // so the lighting shader multiplies by 1.0 (no occlusion) as a safe default.
    if (m_ssaoBlurView != VK_NULL_HANDLE && m_ssaoSampler != VK_NULL_HANDLE) {
        imageInfos[7].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[7].imageView   = m_ssaoBlurView;
        imageInfos[7].sampler     = m_ssaoSampler;
    } else if (m_defaultTexture) {
        imageInfos[7].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[7].imageView   = m_defaultTexture->GetImageView();
        imageInfos[7].sampler     = m_defaultTexture->GetSampler();
    }

    std::array<VkWriteDescriptorSet, 8> writes{};
    for (uint32_t b = 0; b < 8; ++b) {
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

// ============================================================
// CreateDeferredCBResources — UBO for InvViewProj + screen size
// ============================================================
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

// ============================================================
// CreatePBRMaterialResources
// Creates the per-frame PBR material descriptor set (set 0 in GBuffer pass).
// Layout: bindings 0-5 = combined image samplers (diffuse/normal/metal/rough/ao/emit)
//         binding 6   = UBO (PBRMaterialParams, 96 bytes)
// Also creates m_gbufferGeomLayout used by the GBuffer pipeline.
// ============================================================
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

    // --- Descriptor Pool: samplers + UBOs, PBR_SET_COUNT ring sets ---
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 6 * PBR_SET_COUNT;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1 * PBR_SET_COUNT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    poolInfo.maxSets       = PBR_SET_COUNT;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pbrMaterialPool) != VK_SUCCESS) {
        SLEAK_ERROR("PBR: Failed to create PBR material pool!");
        return false;
    }

    // --- Allocate the full ring of descriptor sets ---
    std::array<VkDescriptorSetLayout, PBR_SET_COUNT> layouts;
    layouts.fill(m_pbrMaterialDSL);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pbrMaterialPool;
    allocInfo.descriptorSetCount = PBR_SET_COUNT;
    allocInfo.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(device, &allocInfo, m_pbrMaterialSets.data()) != VK_SUCCESS) {
        SLEAK_ERROR("PBR: Failed to allocate PBR material descriptor sets!");
        return false;
    }

    // --- Per-frame material params UBO: PBR_SETS_PER_FRAME slots, offset-addressed ---
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    VkDeviceSize minAlign = props.limits.minUniformBufferOffsetAlignment;
    VkDeviceSize stride = sizeof(PBRMaterialParams);
    if (minAlign > 0)
        stride = ((stride + minAlign - 1) / minAlign) * minAlign;
    m_pbrMaterialUBOStride = stride;
    const VkDeviceSize uboSize = stride * PBR_SETS_PER_FRAME;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size        = uboSize;
        bufInfo.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufInfo, nullptr, &m_pbrMaterialCBBuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("PBR: Failed to create material UBO buffer {}!", i);
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, m_pbrMaterialCBBuffers[i], &memReqs);
        VkMemoryAllocateInfo memInfo{};
        memInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memInfo.allocationSize  = memReqs.size;
        memInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device, &memInfo, nullptr, &m_pbrMaterialCBMemory[i]) != VK_SUCCESS) {
            SLEAK_ERROR("PBR: Failed to allocate material UBO memory {}!", i);
            return false;
        }
        vkBindBufferMemory(device, m_pbrMaterialCBBuffers[i], m_pbrMaterialCBMemory[i], 0);
        vkMapMemory(device, m_pbrMaterialCBMemory[i], 0, uboSize, 0, &m_pbrMaterialCBMapped[i]);
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

// ============================================================
// BindPBRMaterial
// Updates the per-frame PBR material descriptor set with all 6 textures
// and the material params UBO, then binds set 0 via m_gbufferGeomLayout.
// ============================================================
void VulkanRenderer::BindPBRMaterial(Sleak::Material* material) {
    if (!bFrameStarted || !m_pbrMaterialResourcesCreated || !material) return;

    // Switch to GBuffer pipeline (non-voxel) and reset voxel flag
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufferPipeline);
    m_inVoxelPass = false;

    // Claim this material's own ring slot (own set + own UBO region) so the
    // set is never rewritten while already bound by a prior draw this frame.
    uint32_t slot = m_pbrMaterialSlot[currentFrame];
    if (slot >= PBR_SETS_PER_FRAME) slot = PBR_SETS_PER_FRAME - 1;  // clamp (rare)
    const uint32_t setIdx = currentFrame * PBR_SETS_PER_FRAME + slot;
    const VkDeviceSize uboOffset = slot * m_pbrMaterialUBOStride;

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

    if (m_pbrMaterialCBMapped[currentFrame])
        memcpy(static_cast<char*>(m_pbrMaterialCBMapped[currentFrame]) + uboOffset,
               &params, sizeof(params));

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
    bufInfo.buffer = m_pbrMaterialCBBuffers[currentFrame];
    bufInfo.offset = uboOffset;
    bufInfo.range  = sizeof(PBRMaterialParams);

    std::array<VkWriteDescriptorSet, 7> writes{};
    for (uint32_t i = 0; i < 6; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_pbrMaterialSets[setIdx];
        writes[i].dstBinding      = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imageInfos[i];
    }
    writes[6].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet          = m_pbrMaterialSets[setIdx];
    writes[6].dstBinding      = 6;
    writes[6].dstArrayElement = 0;
    writes[6].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[6].descriptorCount = 1;
    writes[6].pBufferInfo     = &bufInfo;

    vkUpdateDescriptorSets(device, 7, writes.data(), 0, nullptr);

    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_gbufferGeomLayout, 0, 1,
                            &m_pbrMaterialSets[setIdx], 0, nullptr);

    // Advance the ring for the next material this frame.
    m_pbrMaterialSlot[currentFrame] = slot + 1;
}

// ============================================================
// CreateIBLResources
// Creates stub (1×1 black) IBL images with IBLEnabled=0.
// Call TriggerIBLPrecompute() when a skybox cubemap is available.
// ============================================================
bool VulkanRenderer::CreateIBLResources() {
    if (m_iblResourcesCreated) return true;

    // --- Helper: create a 1×1 black 2D or cube image as a stub ---
    auto createStubImage = [&](uint32_t layers, VkFormat fmt,
                               VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo info{};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.extent        = {1, 1, 1};
        info.mipLevels     = 1;
        info.arrayLayers   = layers;
        info.format        = fmt;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                           | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (layers == 6) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

        if (vkCreateImage(device, &info, nullptr, &img) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, img, &memReqs);
        VkMemoryAllocateInfo memInfo{};
        memInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memInfo.allocationSize  = memReqs.size;
        memInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &memInfo, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(device, img, mem, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                           = img;
        viewInfo.viewType                        = (layers == 6) ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                  : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                          = fmt;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = layers;
        return vkCreateImageView(device, &viewInfo, nullptr, &view) == VK_SUCCESS;
    };

    auto createSampler = [&](bool mips, VkSampler& samp) -> bool {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = mips ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod       = 0.0f;
        si.maxLod       = mips ? static_cast<float>(IBL_PREFILTER_MIP_LEVELS) : 1.0f;
        return vkCreateSampler(device, &si, nullptr, &samp) == VK_SUCCESS;
    };

    constexpr VkFormat hdrFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat lutFmt = VK_FORMAT_R16G16_SFLOAT;

    if (!createStubImage(6, hdrFmt, m_iblIrradianceImage, m_iblIrradianceMemory, m_iblIrradianceView)) {
        SLEAK_ERROR("IBL: Failed to create irradiance image!"); return false;
    }
    if (!createSampler(false, m_iblIrradianceSampler)) {
        SLEAK_ERROR("IBL: Failed to create irradiance sampler!"); return false;
    }

    if (!createStubImage(6, hdrFmt, m_iblPrefilterImage, m_iblPrefilterMemory, m_iblPrefilterView)) {
        SLEAK_ERROR("IBL: Failed to create prefilter image!"); return false;
    }
    if (!createSampler(true, m_iblPrefilterSampler)) {
        SLEAK_ERROR("IBL: Failed to create prefilter sampler!"); return false;
    }

    if (!createStubImage(1, lutFmt, m_iblBrdfLutImage, m_iblBrdfLutMemory, m_iblBrdfLutView)) {
        SLEAK_ERROR("IBL: Failed to create BRDF LUT image!"); return false;
    }
    if (!createSampler(false, m_iblBrdfLutSampler)) {
        SLEAK_ERROR("IBL: Failed to create BRDF LUT sampler!"); return false;
    }

    // Transition stub images to SHADER_READ_ONLY_OPTIMAL so they can be sampled
    {
        VkCommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAlloc.commandPool        = commands;
        cmdAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        auto transitionImage = [&](VkImage img, uint32_t layers) {
            VkImageMemoryBarrier bar{};
            bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image               = img;
            bar.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
            bar.srcAccessMask       = 0;
            bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &bar);
        };
        transitionImage(m_iblIrradianceImage, 6);
        transitionImage(m_iblPrefilterImage,  6);
        transitionImage(m_iblBrdfLutImage,    1);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence;
        vkCreateFence(device, &fenceInfo, nullptr, &fence);
        vkQueueSubmit(graphicsQueue, 1, &submit, fence);
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, commands, 1, &cmd);
    }

    // --- Descriptor Set Layout: 3 samplerCubes + 1 sampler2D + 1 UBO ---
    std::array<VkDescriptorSetLayoutBinding, 4> iblBindings{};
    // binding 0: irradiance cubemap
    iblBindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // binding 1: prefilter cubemap
    iblBindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // binding 2: BRDF LUT
    iblBindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // binding 3: IBLSettings UBO
    iblBindings[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo iblDSLInfo{};
    iblDSLInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    iblDSLInfo.bindingCount = static_cast<uint32_t>(iblBindings.size());
    iblDSLInfo.pBindings    = iblBindings.data();
    if (vkCreateDescriptorSetLayout(device, &iblDSLInfo, nullptr, &m_iblDSL) != VK_SUCCESS) {
        SLEAK_ERROR("IBL: Failed to create IBL DSL!"); return false;
    }

    // --- IBL Descriptor Pool ---
    std::array<VkDescriptorPoolSize, 2> iblPoolSizes{};
    iblPoolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * MAX_FRAMES_IN_FLIGHT};
    iblPoolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo iblPoolInfo{};
    iblPoolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    iblPoolInfo.poolSizeCount = static_cast<uint32_t>(iblPoolSizes.size());
    iblPoolInfo.pPoolSizes    = iblPoolSizes.data();
    iblPoolInfo.maxSets       = MAX_FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(device, &iblPoolInfo, nullptr, &m_iblPool) != VK_SUCCESS) {
        SLEAK_ERROR("IBL: Failed to create IBL pool!"); return false;
    }

    // --- Allocate descriptor sets ---
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> iblLayouts;
    iblLayouts.fill(m_iblDSL);
    VkDescriptorSetAllocateInfo iblAlloc{};
    iblAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    iblAlloc.descriptorPool     = m_iblPool;
    iblAlloc.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    iblAlloc.pSetLayouts        = iblLayouts.data();
    if (vkAllocateDescriptorSets(device, &iblAlloc, m_iblSets.data()) != VK_SUCCESS) {
        SLEAK_ERROR("IBL: Failed to allocate IBL descriptor sets!"); return false;
    }

    // --- Per-frame IBL settings UBO (IBLSettingsGPUData, 16 bytes) ---
    constexpr VkDeviceSize settingsSize = sizeof(IBLSettingsGPUData);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size        = settingsSize;
        bufInfo.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufInfo, nullptr, &m_iblSettingsBuffers[i]) != VK_SUCCESS) {
            SLEAK_ERROR("IBL: Failed to create settings buffer {}!", i); return false;
        }
        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, m_iblSettingsBuffers[i], &memReqs);
        VkMemoryAllocateInfo memInfo{};
        memInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memInfo.allocationSize  = memReqs.size;
        memInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device, &memInfo, nullptr, &m_iblSettingsMemory[i]) != VK_SUCCESS) {
            SLEAK_ERROR("IBL: Failed to allocate settings memory {}!", i); return false;
        }
        vkBindBufferMemory(device, m_iblSettingsBuffers[i], m_iblSettingsMemory[i], 0);
        vkMapMemory(device, m_iblSettingsMemory[i], 0, settingsSize, 0, &m_iblSettingsMapped[i]);

        // Default: IBL disabled — lighting shader falls back to hemisphere ambient
        IBLSettingsGPUData settings{};
        settings.IBLEnabled       = 0;
        settings.IBLIntensity     = 1.0f;
        settings.MaxReflectionLOD = static_cast<float>(IBL_PREFILTER_MIP_LEVELS - 1);
        memcpy(m_iblSettingsMapped[i], &settings, sizeof(settings));
    }

    // --- Write initial descriptor sets ---
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorImageInfo irradianceInfo{};
        irradianceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        irradianceInfo.imageView   = m_iblIrradianceView;
        irradianceInfo.sampler     = m_iblIrradianceSampler;

        VkDescriptorImageInfo prefilterInfo{};
        prefilterInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        prefilterInfo.imageView   = m_iblPrefilterView;
        prefilterInfo.sampler     = m_iblPrefilterSampler;

        VkDescriptorImageInfo lutInfo{};
        lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        lutInfo.imageView   = m_iblBrdfLutView;
        lutInfo.sampler     = m_iblBrdfLutSampler;

        VkDescriptorBufferInfo settingsBufInfo{};
        settingsBufInfo.buffer = m_iblSettingsBuffers[i];
        settingsBufInfo.offset = 0;
        settingsBufInfo.range  = sizeof(IBLSettingsGPUData);

        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     m_iblSets[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     &irradianceInfo, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     m_iblSets[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     &prefilterInfo, nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     m_iblSets[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     &lutInfo, nullptr, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     m_iblSets[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     nullptr, &settingsBufInfo, nullptr};
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    m_iblResourcesCreated = true;
    m_iblReady            = false;  // not yet precomputed
    SLEAK_INFO("VulkanRenderer: IBL resources created (IBL disabled until skybox precompute)");
    return true;
}

// ============================================================
// CleanupIBLResources
// ============================================================
void VulkanRenderer::CleanupIBLResources() {
    if (!m_iblResourcesCreated) return;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_iblSettingsMapped[i]) {
            vkUnmapMemory(device, m_iblSettingsMemory[i]);
            m_iblSettingsMapped[i] = nullptr;
        }
        if (m_iblSettingsBuffers[i]) {
            vkDestroyBuffer(device, m_iblSettingsBuffers[i], nullptr);
            m_iblSettingsBuffers[i] = VK_NULL_HANDLE;
        }
        if (m_iblSettingsMemory[i]) {
            vkFreeMemory(device, m_iblSettingsMemory[i], nullptr);
            m_iblSettingsMemory[i] = VK_NULL_HANDLE;
        }
    }

    if (m_iblPool) { vkDestroyDescriptorPool(device, m_iblPool, nullptr); m_iblPool = VK_NULL_HANDLE; }
    if (m_iblDSL)  { vkDestroyDescriptorSetLayout(device, m_iblDSL, nullptr); m_iblDSL = VK_NULL_HANDLE; }

    if (m_iblBrdfLutSampler)  { vkDestroySampler(device, m_iblBrdfLutSampler, nullptr);  m_iblBrdfLutSampler  = VK_NULL_HANDLE; }
    if (m_iblBrdfLutView)     { vkDestroyImageView(device, m_iblBrdfLutView, nullptr);   m_iblBrdfLutView     = VK_NULL_HANDLE; }
    if (m_iblBrdfLutImage)    { vkDestroyImage(device, m_iblBrdfLutImage, nullptr);      m_iblBrdfLutImage    = VK_NULL_HANDLE; }
    if (m_iblBrdfLutMemory)   { vkFreeMemory(device, m_iblBrdfLutMemory, nullptr);       m_iblBrdfLutMemory   = VK_NULL_HANDLE; }

    if (m_iblPrefilterSampler)  { vkDestroySampler(device, m_iblPrefilterSampler, nullptr);  m_iblPrefilterSampler  = VK_NULL_HANDLE; }
    if (m_iblPrefilterView)     { vkDestroyImageView(device, m_iblPrefilterView, nullptr);   m_iblPrefilterView     = VK_NULL_HANDLE; }
    if (m_iblPrefilterImage)    { vkDestroyImage(device, m_iblPrefilterImage, nullptr);      m_iblPrefilterImage    = VK_NULL_HANDLE; }
    if (m_iblPrefilterMemory)   { vkFreeMemory(device, m_iblPrefilterMemory, nullptr);       m_iblPrefilterMemory   = VK_NULL_HANDLE; }

    if (m_iblIrradianceSampler)  { vkDestroySampler(device, m_iblIrradianceSampler, nullptr);  m_iblIrradianceSampler  = VK_NULL_HANDLE; }
    if (m_iblIrradianceView)     { vkDestroyImageView(device, m_iblIrradianceView, nullptr);   m_iblIrradianceView     = VK_NULL_HANDLE; }
    if (m_iblIrradianceImage)    { vkDestroyImage(device, m_iblIrradianceImage, nullptr);      m_iblIrradianceImage    = VK_NULL_HANDLE; }
    if (m_iblIrradianceMemory)   { vkFreeMemory(device, m_iblIrradianceMemory, nullptr);       m_iblIrradianceMemory   = VK_NULL_HANDLE; }

    m_iblResourcesCreated = false;
    m_iblReady            = false;
}

// ============================================================
// TriggerIBLPrecompute
// Runs the offline IBL precompute (BRDF LUT + irradiance + prefilter)
// using the provided environment cubemap. Call from UpdateSkyboxDescriptorSets().
//
// IBL BRDF LUT: fullscreen quad → 512×512 R16G16_SFLOAT
// Irradiance:   6 cube faces → 32×32 R16G16B16A16_SFLOAT cubemap
// Prefilter:    6 faces × IBL_PREFILTER_MIP_LEVELS → 128×128 R16G16B16A16_SFLOAT cubemap
// ============================================================
bool VulkanRenderer::TriggerIBLPrecompute(VkImageView envCubemapView, VkSampler envSampler) {
    if (!m_iblResourcesCreated || envCubemapView == VK_NULL_HANDLE) return false;

    // If called during active frame recording, defer to the start of the next
    // frame. Destroying temp Vulkan objects (render passes, pipelines,
    // framebuffers) while the command buffer is in the recording state
    // invalidates that command buffer.
    if (bFrameStarted) {
        m_iblPrecomputePending      = true;
        m_pendingIBLCubemapView    = envCubemapView;
        m_pendingIBLCubemapSampler = envSampler;
        return true;
    }

    vkDeviceWaitIdle(device);

    // ---- Constants ----
    constexpr uint32_t LUT_SIZE         = 512;
    constexpr uint32_t IRRADIANCE_SIZE  = 32;
    constexpr uint32_t PREFILTER_SIZE   = 128;
    constexpr VkFormat HDR_FMT          = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat LUT_FMT          = VK_FORMAT_R16G16_SFLOAT;

    // ---- Cube face view-projection matrices ----
    // 90° FOV, aspect 1:1, right-handed, Vulkan clip space (depth 0..1)
    // Computed manually to avoid dependency on external math library conventions.
    // Column-major storage matching Sleak::Math::Matrix4 layout.
    struct FaceCapture { float vp[16]; };
    static const float captureProj[16] = {
        1.0f, 0.0f,  0.0f, 0.0f,  // col 0
        0.0f, 1.0f,  0.0f, 0.0f,  // col 1
        0.0f, 0.0f,  1.0f, 1.0f,  // col 2 (near=0.1 in depth, simplified)
        0.0f, 0.0f, -0.0f, 0.0f   // col 3
    };
    (void)captureProj;

    // Use the Math::Matrix4 helpers for correct column-major matrices
    using M4 = Math::Matrix4;
    using V3f = Math::Vector<float, 3>;
    const M4 proj = M4::Perspective(3.14159265f * 0.5f, 1.0f, 0.1f, 10.0f);

    struct FaceVP { float data[16]; };
    FaceVP faceVPs[6];

    // For Vulkan Y-flip we negate the Y component of the up vectors
    const V3f eyes[6]    = {V3f{0,0,0},V3f{0,0,0},V3f{0,0,0},V3f{0,0,0},V3f{0,0,0},V3f{0,0,0}};
    const V3f targets[6] = {V3f{1,0,0},V3f{-1,0,0},V3f{0,1,0},V3f{0,-1,0},V3f{0,0,1},V3f{0,0,-1}};
    const V3f ups[6]     = {V3f{0,-1,0},V3f{0,-1,0},V3f{0,0,1},V3f{0,0,-1},V3f{0,-1,0},V3f{0,-1,0}};

    for (int f = 0; f < 6; ++f) {
        M4 view = M4::LookAt(eyes[f], targets[f], ups[f]);
        M4 vp   = view * proj;
        memcpy(faceVPs[f].data, &vp, sizeof(float) * 16);
    }

    // ---- Recreate IBL images at proper sizes ----
    auto destroyImage = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        if (view)   { vkDestroyImageView(device, view, nullptr);   view = VK_NULL_HANDLE; }
        if (img)    { vkDestroyImage(device, img, nullptr);        img  = VK_NULL_HANDLE; }
        if (mem)    { vkFreeMemory(device, mem, nullptr);          mem  = VK_NULL_HANDLE; }
    };

    auto createCubemapFull = [&](uint32_t size, uint32_t mips, VkFormat fmt,
                                  VkImage& img, VkDeviceMemory& mem, VkImageView& view,
                                  VkSampler& samp, float maxLod) -> bool {
        destroyImage(img, mem, view);
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.extent        = {size, size, 1};
        ci.mipLevels     = mips;
        ci.arrayLayers   = 6;
        ci.format        = fmt;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &ci, nullptr, &img) != VK_SUCCESS) return false;

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(device, img, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(device, img, mem, 0);

        VkImageViewCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image                           = img;
        vi.viewType                        = VK_IMAGE_VIEW_TYPE_CUBE;
        vi.format                          = fmt;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel   = 0;
        vi.subresourceRange.levelCount     = mips;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 6;
        if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) return false;

        if (samp) { vkDestroySampler(device, samp, nullptr); samp = VK_NULL_HANDLE; }
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod       = 0.0f;
        si.maxLod       = maxLod;
        return vkCreateSampler(device, &si, nullptr, &samp) == VK_SUCCESS;
    };

    auto create2DFull = [&](uint32_t w, uint32_t h, VkFormat fmt,
                             VkImage& img, VkDeviceMemory& mem, VkImageView& view,
                             VkSampler& samp) -> bool {
        destroyImage(img, mem, view);
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.extent        = {w, h, 1};
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        ci.format        = fmt;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &ci, nullptr, &img) != VK_SUCCESS) return false;

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(device, img, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(device, img, mem, 0);

        VkImageViewCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image                           = img;
        vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vi.format                          = fmt;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel   = 0;
        vi.subresourceRange.levelCount     = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) return false;

        if (samp) { vkDestroySampler(device, samp, nullptr); samp = VK_NULL_HANDLE; }
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod = 0.0f; si.maxLod = 1.0f;
        return vkCreateSampler(device, &si, nullptr, &samp) == VK_SUCCESS;
    };

    if (!createCubemapFull(IRRADIANCE_SIZE, 1, HDR_FMT,
                            m_iblIrradianceImage, m_iblIrradianceMemory, m_iblIrradianceView,
                            m_iblIrradianceSampler, 1.0f)) {
        SLEAK_ERROR("IBL: Failed to create irradiance cubemap!"); return false;
    }
    if (!createCubemapFull(PREFILTER_SIZE, IBL_PREFILTER_MIP_LEVELS, HDR_FMT,
                            m_iblPrefilterImage, m_iblPrefilterMemory, m_iblPrefilterView,
                            m_iblPrefilterSampler, static_cast<float>(IBL_PREFILTER_MIP_LEVELS))) {
        SLEAK_ERROR("IBL: Failed to create prefilter cubemap!"); return false;
    }
    if (!create2DFull(LUT_SIZE, LUT_SIZE, LUT_FMT,
                      m_iblBrdfLutImage, m_iblBrdfLutMemory, m_iblBrdfLutView,
                      m_iblBrdfLutSampler)) {
        SLEAK_ERROR("IBL: Failed to create BRDF LUT!"); return false;
    }

    // ---- Helper: build a simple render pass for one color attachment ----
    auto makeRenderPass = [&](VkFormat fmt) -> VkRenderPass {
        VkAttachmentDescription att{};
        att.format         = fmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = 0;
        dep.dstSubpass    = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 1;
        rpi.pAttachments    = &att;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sp;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;

        VkRenderPass rp = VK_NULL_HANDLE;
        vkCreateRenderPass(device, &rpi, nullptr, &rp);
        return rp;
    };

    // ---- Helper: build a pipeline for cube-face rendering ----
    // DSL: binding 0 = samplerCube (env map)
    VkDescriptorSetLayoutBinding envBind{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                          VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo envDSLInfo{};
    envDSLInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    envDSLInfo.bindingCount = 1;
    envDSLInfo.pBindings    = &envBind;
    VkDescriptorSetLayout envDSL = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(device, &envDSLInfo, nullptr, &envDSL);

    // Pipeline layout: set 0 = envDSL, push constant = mat4 (+ optional float)
    VkPushConstantRange pcFull{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 68};
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &envDSL;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcFull;
    VkPipelineLayout cubePL = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &plInfo, nullptr, &cubePL);

    // LUT pipeline layout: no descriptor sets, push constant = none
    VkPipelineLayout lutPL = VK_NULL_HANDLE;
    {
        VkPipelineLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        vkCreatePipelineLayout(device, &li, nullptr, &lutPL);
    }

    // Pool + descriptor set for environment cubemap binding
    VkDescriptorPoolSize envPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo envPoolInfo{};
    envPoolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    envPoolInfo.poolSizeCount = 1;
    envPoolInfo.pPoolSizes    = &envPoolSize;
    envPoolInfo.maxSets       = 1;
    VkDescriptorPool envPool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(device, &envPoolInfo, nullptr, &envPool);

    VkDescriptorSetAllocateInfo envAlloc{};
    envAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    envAlloc.descriptorPool     = envPool;
    envAlloc.descriptorSetCount = 1;
    envAlloc.pSetLayouts        = &envDSL;
    VkDescriptorSet envSet = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(device, &envAlloc, &envSet);

    VkDescriptorImageInfo envImgInfo{};
    envImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    envImgInfo.imageView   = envCubemapView;
    envImgInfo.sampler     = envSampler;
    VkWriteDescriptorSet envWrite{};
    envWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    envWrite.dstSet          = envSet;
    envWrite.dstBinding      = 0;
    envWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    envWrite.descriptorCount = 1;
    envWrite.pImageInfo      = &envImgInfo;
    vkUpdateDescriptorSets(device, 1, &envWrite, 0, nullptr);

    // Helper: unit cube vertex data (36 triangle vertices)
    static const float cubeVerts[] = {
        -1,-1,-1, +1,-1,-1, +1,+1,-1,  +1,+1,-1, -1,+1,-1, -1,-1,-1,
        -1,-1,+1, +1,-1,+1, +1,+1,+1,  +1,+1,+1, -1,+1,+1, -1,-1,+1,
        -1,+1,+1, -1,+1,-1, -1,-1,-1,  -1,-1,-1, -1,-1,+1, -1,+1,+1,
        +1,+1,+1, +1,+1,-1, +1,-1,-1,  +1,-1,-1, +1,-1,+1, +1,+1,+1,
        -1,-1,-1, +1,-1,-1, +1,-1,+1,  +1,-1,+1, -1,-1,+1, -1,-1,-1,
        -1,+1,-1, +1,+1,-1, +1,+1,+1,  +1,+1,+1, -1,+1,+1, -1,+1,-1
    };
    VkBuffer cubeVBO = VK_NULL_HANDLE;
    VkDeviceMemory cubeVBOMem = VK_NULL_HANDLE;
    {
        constexpr VkDeviceSize cubeVBOSize = sizeof(cubeVerts);
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = cubeVBOSize;
        bi.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bi, nullptr, &cubeVBO);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device, cubeVBO, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &mai, nullptr, &cubeVBOMem);
        vkBindBufferMemory(device, cubeVBO, cubeVBOMem, 0);
        void* ptr;
        vkMapMemory(device, cubeVBOMem, 0, cubeVBOSize, 0, &ptr);
        memcpy(ptr, cubeVerts, cubeVBOSize);
        vkUnmapMemory(device, cubeVBOMem);
    }

    // Helper: build a pipeline for cube rendering or LUT generation
    auto makePipeline = [&](VkShaderModule vert, VkShaderModule frag,
                             VkPipelineLayout layout, VkRenderPass rp,
                             uint32_t width, uint32_t height,
                             bool hasVertexInput, bool dynamicViewport = false) -> VkPipeline {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        VkVertexInputBindingDescription vib{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription via{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo vis{};
        vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if (hasVertexInput) {
            vis.vertexBindingDescriptionCount   = 1;
            vis.pVertexBindingDescriptions      = &vib;
            vis.vertexAttributeDescriptionCount = 1;
            vis.pVertexAttributeDescriptions    = &via;
        }

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport vp{0,0,(float)width,(float)height,0,1};
        VkRect2D sc{{0,0},{width,height}};
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.pViewports    = dynamicViewport ? nullptr : &vp;
        vps.scissorCount  = 1;
        vps.pScissors     = dynamicViewport ? nullptr : &sc;

        VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dynStates;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cbs{};
        cbs.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 1;
        cbs.pAttachments    = &cba;

        VkGraphicsPipelineCreateInfo pi{};
        pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pi.stageCount          = 2;
        pi.pStages             = stages;
        pi.pVertexInputState   = &vis;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState      = &vps;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState   = &ms;
        pi.pDepthStencilState  = &ds;
        pi.pColorBlendState    = &cbs;
        pi.pDynamicState       = dynamicViewport ? &dyn : nullptr;
        pi.layout              = layout;
        pi.renderPass          = rp;
        pi.subpass             = 0;

        VkPipeline p = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
        return p;
    };

    // Helper: load SPIR-V module
    auto loadModule = [&](const char* path) -> VkShaderModule {
        VulkanShader tmp(device);
        return tmp.LoadSPIRV(path);
    };

    // ======================== COMMAND BUFFER ========================
    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool        = commands;
    cmdAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);
    VkCommandBufferBeginInfo cmdBegin{};
    cmdBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmdBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cmdBegin);

    // ======================== BRDF LUT ========================
    {
        VkShaderModule lutVert = loadModule("assets/shaders/ibl_brdf_lut.vert.spv");
        VkShaderModule lutFrag = loadModule("assets/shaders/ibl_brdf_lut.frag.spv");
        VkRenderPass   lutRP   = makeRenderPass(LUT_FMT);

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = lutRP;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &m_iblBrdfLutView;
        fbInfo.width           = LUT_SIZE;
        fbInfo.height          = LUT_SIZE;
        fbInfo.layers          = 1;
        VkFramebuffer lutFB = VK_NULL_HANDLE;
        vkCreateFramebuffer(device, &fbInfo, nullptr, &lutFB);

        VkPipeline lutPipe = makePipeline(lutVert, lutFrag, lutPL, lutRP,
                                          LUT_SIZE, LUT_SIZE, false);

        VkClearValue cv{};
        VkRenderPassBeginInfo rpb{};
        rpb.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass        = lutRP;
        rpb.framebuffer       = lutFB;
        rpb.renderArea.extent = {LUT_SIZE, LUT_SIZE};
        rpb.clearValueCount   = 1;
        rpb.pClearValues      = &cv;
        vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lutPipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);

        vkDestroyPipeline(device, lutPipe, nullptr);
        vkDestroyFramebuffer(device, lutFB, nullptr);
        vkDestroyRenderPass(device, lutRP, nullptr);
        vkDestroyShaderModule(device, lutVert, nullptr);
        vkDestroyShaderModule(device, lutFrag, nullptr);
    }

    // ======================== IRRADIANCE ========================
    {
        VkShaderModule irrVert = loadModule("assets/shaders/ibl_irradiance.vert.spv");
        VkShaderModule irrFrag = loadModule("assets/shaders/ibl_irradiance.frag.spv");
        VkRenderPass   irrRP   = makeRenderPass(HDR_FMT);

        VkPipeline irrPipe = makePipeline(irrVert, irrFrag, cubePL, irrRP,
                                          IRRADIANCE_SIZE, IRRADIANCE_SIZE, true);

        for (uint32_t face = 0; face < 6; ++face) {
            VkImageViewCreateInfo faceViewInfo{};
            faceViewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            faceViewInfo.image                           = m_iblIrradianceImage;
            faceViewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            faceViewInfo.format                          = HDR_FMT;
            faceViewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            faceViewInfo.subresourceRange.baseMipLevel   = 0;
            faceViewInfo.subresourceRange.levelCount     = 1;
            faceViewInfo.subresourceRange.baseArrayLayer = face;
            faceViewInfo.subresourceRange.layerCount     = 1;
            VkImageView faceView = VK_NULL_HANDLE;
            vkCreateImageView(device, &faceViewInfo, nullptr, &faceView);

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = irrRP;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = &faceView;
            fbInfo.width           = IRRADIANCE_SIZE;
            fbInfo.height          = IRRADIANCE_SIZE;
            fbInfo.layers          = 1;
            VkFramebuffer faceFB = VK_NULL_HANDLE;
            vkCreateFramebuffer(device, &fbInfo, nullptr, &faceFB);

            VkClearValue cv{};
            VkRenderPassBeginInfo rpb{};
            rpb.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpb.renderPass        = irrRP;
            rpb.framebuffer       = faceFB;
            rpb.renderArea.extent = {IRRADIANCE_SIZE, IRRADIANCE_SIZE};
            rpb.clearValueCount   = 1;
            rpb.pClearValues      = &cv;
            vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, irrPipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    cubePL, 0, 1, &envSet, 0, nullptr);
            vkCmdPushConstants(cmd, cubePL,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, 64, faceVPs[face].data);
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVBO, &off);
            vkCmdDraw(cmd, 36, 1, 0, 0);
            vkCmdEndRenderPass(cmd);

            vkDestroyFramebuffer(device, faceFB, nullptr);
            vkDestroyImageView(device, faceView, nullptr);
        }

        vkDestroyPipeline(device, irrPipe, nullptr);
        vkDestroyRenderPass(device, irrRP, nullptr);
        vkDestroyShaderModule(device, irrVert, nullptr);
        vkDestroyShaderModule(device, irrFrag, nullptr);
    }

    // ======================== PREFILTER ========================
    {
        VkShaderModule pfVert = loadModule("assets/shaders/ibl_prefilter.vert.spv");
        VkShaderModule pfFrag = loadModule("assets/shaders/ibl_prefilter.frag.spv");
        VkRenderPass   pfRP   = makeRenderPass(HDR_FMT);

        VkPipeline pfPipe = makePipeline(pfVert, pfFrag, cubePL, pfRP,
                                         PREFILTER_SIZE, PREFILTER_SIZE, true, true);

        for (uint32_t mip = 0; mip < IBL_PREFILTER_MIP_LEVELS; ++mip) {
            uint32_t mipSize = std::max(1u, PREFILTER_SIZE >> mip);
            float roughness  = static_cast<float>(mip) / static_cast<float>(IBL_PREFILTER_MIP_LEVELS - 1);

            for (uint32_t face = 0; face < 6; ++face) {
                VkImageViewCreateInfo faceViewInfo{};
                faceViewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                faceViewInfo.image                           = m_iblPrefilterImage;
                faceViewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
                faceViewInfo.format                          = HDR_FMT;
                faceViewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                faceViewInfo.subresourceRange.baseMipLevel   = mip;
                faceViewInfo.subresourceRange.levelCount     = 1;
                faceViewInfo.subresourceRange.baseArrayLayer = face;
                faceViewInfo.subresourceRange.layerCount     = 1;
                VkImageView faceView = VK_NULL_HANDLE;
                vkCreateImageView(device, &faceViewInfo, nullptr, &faceView);

                VkFramebufferCreateInfo fbInfo{};
                fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                fbInfo.renderPass      = pfRP;
                fbInfo.attachmentCount = 1;
                fbInfo.pAttachments    = &faceView;
                fbInfo.width           = mipSize;
                fbInfo.height          = mipSize;
                fbInfo.layers          = 1;
                VkFramebuffer faceFB = VK_NULL_HANDLE;
                vkCreateFramebuffer(device, &fbInfo, nullptr, &faceFB);

                // Viewport must match mip size
                VkViewport mipVP{0,0,(float)mipSize,(float)mipSize,0,1};
                VkRect2D   mipSc{{0,0},{mipSize,mipSize}};
                vkCmdSetViewport(cmd, 0, 1, &mipVP);
                vkCmdSetScissor(cmd, 0, 1, &mipSc);

                VkClearValue cv{};
                VkRenderPassBeginInfo rpb{};
                rpb.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                rpb.renderPass        = pfRP;
                rpb.framebuffer       = faceFB;
                rpb.renderArea.extent = {mipSize, mipSize};
                rpb.clearValueCount   = 1;
                rpb.pClearValues      = &cv;
                vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pfPipe);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        cubePL, 0, 1, &envSet, 0, nullptr);

                // Push constant: mat4 VP (64 bytes) + roughness (4 bytes) = 68 bytes
                struct PrefilterPC { float vp[16]; float roughness; } pc;
                memcpy(pc.vp, faceVPs[face].data, 64);
                pc.roughness = roughness;
                vkCmdPushConstants(cmd, cubePL,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, 68, &pc);

                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVBO, &off);
                vkCmdDraw(cmd, 36, 1, 0, 0);
                vkCmdEndRenderPass(cmd);

                vkDestroyFramebuffer(device, faceFB, nullptr);
                vkDestroyImageView(device, faceView, nullptr);
            }
        }

        vkDestroyPipeline(device, pfPipe, nullptr);
        vkDestroyRenderPass(device, pfRP, nullptr);
        vkDestroyShaderModule(device, pfVert, nullptr);
        vkDestroyShaderModule(device, pfFrag, nullptr);
    }

    // Submit and wait
    vkEndCommandBuffer(cmd);
    VkSubmitInfo sub{};
    sub.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers    = &cmd;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(device, &fi, nullptr, &fence);
    vkQueueSubmit(graphicsQueue, 1, &sub, fence);
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, commands, 1, &cmd);

    // Cleanup temp resources
    vkDestroyBuffer(device, cubeVBO, nullptr);
    vkFreeMemory(device, cubeVBOMem, nullptr);
    vkDestroyDescriptorPool(device, envPool, nullptr);
    vkDestroyDescriptorSetLayout(device, envDSL, nullptr);
    vkDestroyPipelineLayout(device, cubePL, nullptr);
    vkDestroyPipelineLayout(device, lutPL, nullptr);

    // Enable IBL in all per-frame settings UBOs
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        IBLSettingsGPUData settings{};
        settings.IBLEnabled       = 1;
        settings.IBLIntensity     = 1.0f;
        settings.MaxReflectionLOD = static_cast<float>(IBL_PREFILTER_MIP_LEVELS - 1);
        if (m_iblSettingsMapped[i])
            memcpy(m_iblSettingsMapped[i], &settings, sizeof(settings));

        // Re-write IBL descriptor sets with the new real image views
        VkDescriptorImageInfo irr{VK_NULL_HANDLE, m_iblIrradianceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        irr.sampler = m_iblIrradianceSampler;
        VkDescriptorImageInfo pf{VK_NULL_HANDLE, m_iblPrefilterView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        pf.sampler = m_iblPrefilterSampler;
        VkDescriptorImageInfo lut{VK_NULL_HANDLE, m_iblBrdfLutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        lut.sampler = m_iblBrdfLutSampler;
        VkDescriptorBufferInfo sbuf{m_iblSettingsBuffers[i], 0, sizeof(IBLSettingsGPUData)};

        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_iblSets[i], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &irr, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_iblSets[i], 1, 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &pf, nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_iblSets[i], 2, 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &lut, nullptr, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_iblSets[i], 3, 0, 1,
                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &sbuf, nullptr};
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    m_iblReady = true;
    SLEAK_INFO("VulkanRenderer: IBL precompute complete — IBL enabled");
    return true;
}

// ============================================================
// CleanupGBufferResources
// ============================================================
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

// ============================================================
// BindGBufferShader — switches to the GBuffer pipeline
// Called by RenderCommandQueue when deferred is active
// ============================================================
void VulkanRenderer::BindGBufferShader() {
    if (!bFrameStarted || !m_gbufferResourcesCreated) return;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufferPipeline);
    // Reset voxel flag — this binds the default GBuffer pipeline (96-byte Vertex stride).
    // Without this reset, the next voxel draw's BindVertexBuffer sees m_inVoxelPass==true,
    // skips switching to the voxel pipeline, and draws 48-byte VoxelVertex data with
    // a 96-byte stride → corruption.
    m_inVoxelPass = false;
}

// ============================================================
// ExecuteDeferredLightingPass
// ============================================================
void VulkanRenderer::ExecuteDeferredLightingPass() {
    if (!bFrameStarted || !m_gbufferResourcesCreated) return;

    // 1. End GBuffer render pass — transitions color RTs → SHADER_READ_ONLY,
    //    depth → DEPTH_STENCIL_READ_ONLY via finalLayout in CreateGBufferRenderPass
    vkCmdEndRenderPass(command);
    m_inGeometryPass = false;
    m_inVoxelPass = false;  // geometry pass is over; pipeline state doesn't survive across render passes

    // 2. Run SSAO (raw + bilateral blur) using GBuffer normal/worldpos/depth.
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

// ============================================================
// BeginForwardTransparentPass
// ============================================================
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

// ============================================================
// EndForwardTransparentPass
// Do NOT end the render pass here — EndRender will call vkCmdEndRenderPass
// ============================================================
void VulkanRenderer::EndForwardTransparentPass() {
    m_inForwardTransparentPass = false;
    // m_forwardPassOpen stays true — the RP remains open until EndRender
}

// ============================================================
// TAA static helpers — placed here so UpdateDeferredCB can call HaltonSeq.
// ============================================================
static float HaltonSeq(int index, int base) {
    float result = 0.0f;
    float f = 1.0f;
    for (int i = index; i > 0; i /= base) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(i % base);
    }
    return result;
}

// Row-major mat4 multiply: C = A * B
static void MatMul4(const float A[16], const float B[16], float C[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += A[r * 4 + k] * B[k * 4 + c];
            C[r * 4 + c] = s;
        }
}

// Row-major 4x4 inverse via Gauss-Jordan elimination
static bool InvertMat4(const float M[16], float out[16]) {
    float m[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) m[r][c]     = M[r * 4 + c];
        for (int c = 0; c < 4; ++c) m[r][4 + c] = (r == c) ? 1.0f : 0.0f;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = -1; float maxv = 0.0f;
        for (int row = col; row < 4; ++row) {
            float v = std::abs(m[row][col]);
            if (v > maxv) { maxv = v; pivot = row; }
        }
        if (pivot < 0 || maxv < 1e-7f) return false;
        if (pivot != col) std::swap(m[col], m[pivot]);
        float inv = 1.0f / m[col][col];
        for (int c = 0; c < 8; ++c) m[col][c] *= inv;
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            float f = m[row][col];
            for (int c = 0; c < 8; ++c) m[row][c] -= f * m[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = m[r][4 + c];
    return true;
}

// ============================================================
// UpdateDeferredCB — CPU writes InvViewProj + screen size.
// Also snapshots the current View/Projection matrices from Camera so the
// SSAO UBO (populated each frame inside RenderSSAOPasses) reflects the
// same camera as the lighting pass.
// ============================================================
void VulkanRenderer::UpdateDeferredCB(const void* data, uint32_t size) {
    if (!m_deferredCBCreated || !data) return;
    uint32_t copySize = std::min(size, static_cast<uint32_t>(sizeof(DeferredCBData)));
    memcpy(m_deferredCBMapped[currentFrame], data, copySize);

    // Snapshot current camera View / Projection for SSAO/SSR/TAA UBO population.
    const Math::Matrix4& V = Camera::GetMainViewMatrix();
    const Math::Matrix4& P = Camera::GetMainProjectionMatrix();
    memcpy(m_cachedView,       &V(0, 0), sizeof(m_cachedView));
    memcpy(m_cachedProjection, &P(0, 0), sizeof(m_cachedProjection));

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

// ============================================================
// FillFullscreenViewportScissor — set viewport+scissor for a pass.
// ============================================================
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
    // Set 0 for SSAO: bindings 0..3 (gNormalRough, gWorldPos, gDepth, noise).
    {
        std::array<VkDescriptorSetLayoutBinding, 4> binds{};
        for (uint32_t i = 0; i < 4; ++i) {
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
        // Set 0 (input samplers): gNormalRough, gWorldPos, gDepth, noise.
        std::array<VkDescriptorImageInfo, 4> inputInfos{};
        // gNormalRough = gbuffer[1], gWorldPos = gbuffer[3].
        inputInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        inputInfos[0].imageView   = m_gbufferViews[1];
        inputInfos[0].sampler     = m_ssaoPointSampler;

        inputInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        inputInfos[1].imageView   = m_gbufferViews[3];
        inputInfos[1].sampler     = m_ssaoPointSampler;

        inputInfos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        inputInfos[2].imageView   = depthImageView;
        inputInfos[2].sampler     = m_ssaoPointSampler;

        inputInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        inputInfos[3].imageView   = m_ssaoNoiseView;
        inputInfos[3].sampler     = m_ssaoNoiseSampler;

        std::array<VkWriteDescriptorSet, 4> inputWrites{};
        for (uint32_t i = 0; i < 4; ++i) {
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
    memcpy(p.View,       m_cachedView,       sizeof(p.View));
    memcpy(p.Projection, m_cachedProjection, sizeof(p.Projection));

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
    // Set 0: 6 combined image samplers (gNormalRough, gWorldPos, gDepth,
    //         gMetalEmit, gAlbedoAO, sceneHDR).
    {
        std::array<VkDescriptorSetLayoutBinding, 6> binds{};
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
    // Sampler slots: [0]=gNormalRough, [1]=gWorldPos, [2]=gDepth,
    //                [3]=gMetalEmit, [4]=gAlbedoAO, [5]=sceneHDR.
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        std::array<VkDescriptorImageInfo, 6> infos{};

        // gNormalRough = gbuffer[1]
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = m_gbufferViews[1];
        infos[0].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // gWorldPos = gbuffer[3]
        infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[1].imageView   = m_gbufferViews[3];
        infos[1].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // gDepth — READ_ONLY because shadow pass finalLayout is
        // DEPTH_STENCIL_READ_ONLY, GBuffer pass final is READ_ONLY, but the
        // forward pass exits at DEPTH_STENCIL_ATTACHMENT_OPTIMAL. SSR bracket
        // transitions it to READ_ONLY before sampling (see RenderSSRPass).
        infos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[2].imageView   = depthImageView;
        infos[2].sampler     = m_depthSampler ? m_depthSampler : m_ssrSampler;

        // gMetalEmit = gbuffer[2]
        infos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[3].imageView   = m_gbufferViews[2];
        infos[3].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // gAlbedoAO = gbuffer[0]
        infos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[4].imageView   = m_gbufferViews[0];
        infos[4].sampler     = m_gbufferSampler ? m_gbufferSampler : m_ssrSampler;

        // sceneHDR — forward pass finalLayout is SHADER_READ_ONLY_OPTIMAL.
        infos[5].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[5].imageView   = m_hdrSceneView;
        infos[5].sampler     = m_ssrSampler;

        std::array<VkWriteDescriptorSet, 6> writes{};
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
    memcpy(p.View,       m_cachedView,       sizeof(p.View));
    memcpy(p.Projection, m_cachedProjection, sizeof(p.Projection));

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
    pc.exposure      = 1.0f;
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
