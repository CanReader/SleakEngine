#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanBuffer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanTexture.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanCubemapTexture.hpp"

#include <SDL3/SDL_vulkan.h>
#include <Window.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
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
        [this](const void* data, uint32_t w, uint32_t h, TextureFormat fmt) -> Texture* {
            auto* tex = new VulkanTexture(device, physicalDevice, commands, graphicsQueue);
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

    if (!CreateFrameBuffer())
        SLEAK_RETURN_ERR("Failed to create framebuffer of renderer!");

    if (!CreateSyncObjects())
        SLEAK_RETURN_ERR("Failed to synchronization objects of renderer!");

    SLEAK_INFO("Vulkan renderer has been initialized successfully!");

    return true;
}

// BeginRender: prepare the command buffer, begin render pass.
// Do NOT end the command buffer here — the RenderCommandQueue will
// record draw commands via the RenderContext interface.
void VulkanRenderer::BeginRender() {
    bFrameStarted = false;
    if (!bRender)
        return;

    VkResult result;

    if (device && !inFlightFences.empty()) {
        result = vkWaitForFences(device, 1, &inFlightFences[currentFrame],
                                 VK_TRUE, UINT64_MAX);
        result = vkResetFences(device, 1, &inFlightFences[currentFrame]);
    }

    // Acquire the next image from the swapchain
    result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX,
                                    imageAvailableSemaphores[currentFrame],
                                    VK_NULL_HANDLE, &CurrentFrameIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapChain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        SLEAK_ERROR("Failed to acquire swapchain image!");
        return;
    }

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

    // Begin render pass
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0] = clearColor;
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = renderPass;
    passInfo.framebuffer = swapChainFramebuffers[CurrentFrameIndex];
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = scExtent;
    passInfo.clearValueCount =
        static_cast<uint32_t>(clearValues.size());
    passInfo.pClearValues = clearValues.data();

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

    // RenderCommandQueue will now call Draw/DrawIndexed/Bind* methods
    // via the RenderContext interface on this object

    if (bImInitialized) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    bFrameStarted = true;
}

// EndRender: end render pass, end command buffer, submit, present.
void VulkanRenderer::EndRender() {
    if (!bRender || !bFrameStarted)
        return;

    if (bImInitialized) {
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
    }

    vkCmdEndRenderPass(command);

    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
        SLEAK_ERROR("Failed to end command buffer!");
        return;
    }

    // Submit
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command;

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

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

// -----------------------------------------------------------------------
// RenderContext Implementation
// -----------------------------------------------------------------------

void VulkanRenderer::Draw(uint32_t vertexCount) {
    vkCmdDraw(command, vertexCount, 1, 0, 0);
}

void VulkanRenderer::DrawIndexed(uint32_t indexCount) {
    vkCmdDrawIndexed(command, indexCount, 1, 0, 0, 0);
}

void VulkanRenderer::DrawInstance(uint32_t instanceCount,
                                   uint32_t vertexPerInstance) {
    vkCmdDraw(command, vertexPerInstance, instanceCount, 0, 0);
}

void VulkanRenderer::DrawIndexedInstance(uint32_t instanceCount,
                                          uint32_t indexPerInstance) {
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
    auto* vkBuf = dynamic_cast<VulkanBuffer*>(buffer.get());
    if (!vkBuf) return;
    VkBuffer buffers[] = {vkBuf->GetVkBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(command, slot, 1, buffers, offsets);
}

void VulkanRenderer::BindIndexBuffer(RefPtr<BufferBase> buffer,
                                      uint32_t slot) {
    auto* vkBuf = dynamic_cast<VulkanBuffer*>(buffer.get());
    if (!vkBuf) return;
    vkCmdBindIndexBuffer(command, vkBuf->GetVkBuffer(), 0,
                         VK_INDEX_TYPE_UINT32);
}

void VulkanRenderer::BindConstantBuffer(RefPtr<BufferBase> buffer,
                                         uint32_t slot) {
    auto* vkBuf = dynamic_cast<VulkanBuffer*>(buffer.get());
    if (!vkBuf) return;

    // Use push constants — recorded into the command buffer per draw call
    void* data = vkBuf->GetData();
    if (!data) return;

    uint32_t size = static_cast<uint32_t>(vkBuf->GetSize());
    if (size > 128) size = 128;  // Vulkan guarantees at least 128 bytes

    vkCmdPushConstants(command, pipelineLay,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, size, data);
}

BufferBase* VulkanRenderer::CreateBuffer(BufferType type, uint32_t size,
                                          void* data) {
    auto* buffer = new VulkanBuffer(device, physicalDevice, size, type,
                                     commands, graphicsQueue);
    buffer->Initialize(data);
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

Texture* VulkanRenderer::CreateTexture(const std::string& TexturePath) {
    auto* texture = new VulkanTexture(device, physicalDevice, commands,
                                       graphicsQueue);
    if (texture->LoadFromFile(TexturePath)) {
        WriteTextureDescriptors(texture);
        return texture;
    }
    delete texture;
    return nullptr;
}

Texture* VulkanRenderer::CreateTextureFromData(uint32_t width,
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

Texture* VulkanRenderer::CreateCubemapTexture(
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
        for (size_t i = 0; i < skyboxDescriptorSets.size(); i++) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = texture->GetImageView();
            imageInfo.sampler = texture->GetSampler();

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

Texture* VulkanRenderer::CreateCubemapTextureFromPanorama(
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
        for (size_t i = 0; i < skyboxDescriptorSets.size(); i++) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = texture->GetImageView();
            imageInfo.sampler = texture->GetSampler();

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

void VulkanRenderer::BindTexture(RefPtr<Sleak::Texture> texture,
                                  uint32_t slot) {
    if (!texture.IsValid() || slot != 0)
        return;

    // Cast to VulkanTexture to access its per-texture descriptor sets
    auto* vkTex = dynamic_cast<VulkanTexture*>(texture.get());
    if (!vkTex || !vkTex->HasDescriptorSets())
        return;

    const auto& sets = vkTex->GetDescriptorSets();
    if (CurrentFrameIndex < sets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &sets[CurrentFrameIndex], 0, nullptr);
    }
}

void VulkanRenderer::BindTextureRaw(Sleak::Texture* texture, uint32_t slot) {
    if (!texture || slot != 0)
        return;

    auto* vkTex = dynamic_cast<VulkanTexture*>(texture);
    if (!vkTex || !vkTex->HasDescriptorSets())
        return;

    const auto& sets = vkTex->GetDescriptorSets();
    if (CurrentFrameIndex < sets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &sets[CurrentFrameIndex], 0, nullptr);
    }
}

void VulkanRenderer::BeginSkyboxPass() {
    if (skyboxPipeline == VK_NULL_HANDLE || !m_skyboxDescriptorsWritten)
        return;

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      skyboxPipeline);

    if (CurrentFrameIndex < skyboxDescriptorSets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &skyboxDescriptorSets[CurrentFrameIndex], 0, nullptr);
    }
}

void VulkanRenderer::EndSkyboxPass() {
    // Restore main pipeline
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Rebind main texture descriptor sets
    if (m_textureDescriptorsWritten &&
        CurrentFrameIndex < descriptorSets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &descriptorSets[CurrentFrameIndex], 0, nullptr);
    }
}

// -----------------------------------------------------------------------
// Command Buffer
// -----------------------------------------------------------------------

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

// -----------------------------------------------------------------------
// Cleanup
// -----------------------------------------------------------------------

void VulkanRenderer::Cleanup() {
    SLEAK_INFO("Cleaning Vulkan...");

    bRender = false;

    // Wait for the device to finish all work
    if (device) {
        vkDeviceWaitIdle(device);
    }

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

    // Destroy bone UBO resources
    CleanupBoneUBOResources();

    // Destroy descriptor set layouts
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

// -----------------------------------------------------------------------
// Vulkan Instance
// -----------------------------------------------------------------------

bool VulkanRenderer::InitVulkan() {
    try {
        instance = VK_NULL_HANDLE;

        std::vector<const char*> requiredExtensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME
        };

        #ifdef PLATFORM_LINUX
            requiredExtensions.push_back("VK_KHR_wayland_surface");
            requiredExtensions.push_back("VK_KHR_xlib_surface");
        #elif defined(PLATFORM_WIN)
            requiredExtensions.push_back(
                VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        #elif defined(TARGET_OS_MAC) || defined(TARGET_OS_IOS)
            requiredExtensions.push_back(VK_MVK_MOLTENVK_EXTENSION_NAME);
        #endif

        // Check which validation layers are available
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
                SLEAK_WARN("Vulkan validation layer not available, "
                           "running without validation");
                // Also remove debug utils extension if no validation
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

// -----------------------------------------------------------------------
// Device
// -----------------------------------------------------------------------

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

// -----------------------------------------------------------------------
// Debug
// -----------------------------------------------------------------------

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

// -----------------------------------------------------------------------
// Surface
// -----------------------------------------------------------------------

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

// -----------------------------------------------------------------------
// Swap Chain
// -----------------------------------------------------------------------

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
    if (!CreateFrameBuffer()) {
        SLEAK_ERROR("Failed to recreate framebuffers!");
        return false;
    }

    return true;
}

void VulkanRenderer::CleanupSwapChain() {
    CleanupDepthResources();

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

void VulkanRenderer::ConfigureRenderMode() {
    // Pipeline recreation needed for Vulkan polygon mode changes
}

void VulkanRenderer::ConfigureRenderFace() {
    // Pipeline recreation needed for Vulkan cull mode changes
}

// -----------------------------------------------------------------------
// Image Views
// -----------------------------------------------------------------------

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

// -----------------------------------------------------------------------
// Depth Resources
// -----------------------------------------------------------------------

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
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
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

// -----------------------------------------------------------------------
// Descriptor Sets
// -----------------------------------------------------------------------

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

    return true;
}

bool VulkanRenderer::CreateDescriptorPool() {
    uint32_t imageCount =
        static_cast<uint32_t>(swapChainImages.size());

    // Allow up to 128 textures, each needing imageCount descriptor sets
    static constexpr uint32_t MAX_TEXTURES = 128;
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

// -----------------------------------------------------------------------
// Graphics Pipeline
// -----------------------------------------------------------------------

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
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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

    // Two descriptor set layouts: set 0 = texture sampler, set 1 = bone UBO
    std::array<VkDescriptorSetLayout, 2> setLayouts = {
        descriptorSetLayout, boneDescriptorSetLayout
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
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                        &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS)
        SLEAK_ERROR("Failed to create graphics pipeline!!");

    return true;
}

// -----------------------------------------------------------------------
// Render Pass
// -----------------------------------------------------------------------

bool VulkanRenderer::CreateRenderPass() {
    // Color attachment
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = scImageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Depth attachment
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
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

    // Subpass
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

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
    std::array<VkAttachmentDescription, 2> attachments = {
        colorAttachment, depthAttachment};

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

// -----------------------------------------------------------------------
// Frame Buffer
// -----------------------------------------------------------------------

bool VulkanRenderer::CreateFrameBuffer() {
    swapChainFramebuffers.resize(swapChainImageViews.size());

    for (size_t i = 0; i < swapChainImageViews.size(); i++) {
        std::array<VkImageView, 2> attachments = {
            swapChainImageViews[i], depthImageView};

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

// -----------------------------------------------------------------------
// Command Pool + Sync
// -----------------------------------------------------------------------

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
    imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.resize(swapChainImages.size());
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                               &imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr,
                           &inFlightFences[i]) != VK_SUCCESS) {
            SLEAK_RETURN_ERR("Failed to create synchronization objects!");
        }
    }

    for (size_t i = 0; i < swapChainImages.size(); i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                               &renderFinishedSemaphores[i]) != VK_SUCCESS) {
            SLEAK_RETURN_ERR("Failed to create render finished semaphore!");
        }
    }

    return true;
}

// -----------------------------------------------------------------------
// Swapchain Queries
// -----------------------------------------------------------------------

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
    for (auto& format : formats)
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;

    return formats[0];
}

VkPresentModeKHR VulkanRenderer::ChoosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) {
    for (auto& mode : modes)
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
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

// -----------------------------------------------------------------------
// Debug Messenger
// -----------------------------------------------------------------------

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
    if (!device || !instance || !graphicsQueue || !renderPass)
        return false;

    // Create a dedicated descriptor pool for ImGUI
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1;
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
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.RenderPass = renderPass;
    initInfo.Subpass = 0;

    if (!ImGui_ImplVulkan_Init(&initInfo))
        return false;

    bImInitialized = true;
    return true;
}

// -----------------------------------------------------------------------
// Skybox Pipeline
// -----------------------------------------------------------------------

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
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
    pipelineInfo.renderPass = renderPass;
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

// -----------------------------------------------------------------------
// Bone UBO Resources (for skeletal animation)
// -----------------------------------------------------------------------

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

// -----------------------------------------------------------------------
// Skinned Pipeline (for skeletal animation)
// -----------------------------------------------------------------------

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
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
    pipelineInfo.renderPass = renderPass;
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
    if (skinnedPipeline == VK_NULL_HANDLE) {
        if (!CreateSkinnedPipeline()) return;
    }

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      skinnedPipeline);

    // Do NOT rebind texture descriptor sets here — BindMaterialCommand already
    // bound the correct per-material texture at set 0 before this draw call.
    // Vulkan preserves descriptor set bindings across pipeline switches when
    // the pipeline layout is compatible (we reuse the same pipelineLay).
}

void VulkanRenderer::EndSkinnedPass() {
    // Restore main pipeline
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Rebind main texture descriptor sets
    if (m_textureDescriptorsWritten &&
        CurrentFrameIndex < descriptorSets.size()) {
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLay, 0, 1,
            &descriptorSets[CurrentFrameIndex], 0, nullptr);
    }
}

void VulkanRenderer::BindBoneBuffer(RefPtr<BufferBase> buffer) {
    if (!buffer) return;

    // Lazily create bone UBO resources on first use
    if (!m_boneUBOCreated) {
        if (!CreateBoneUBOResources()) return;
    }

    auto* vkBuf = dynamic_cast<VulkanBuffer*>(buffer.get());
    if (!vkBuf) return;

    void* data = vkBuf->GetData();
    if (!data) return;

    uint32_t size = static_cast<uint32_t>(vkBuf->GetSize());
    static constexpr uint32_t MAX_BONE_UBO_SIZE = 256 * 64; // MAX_BONES * sizeof(mat4)
    if (size > MAX_BONE_UBO_SIZE) size = MAX_BONE_UBO_SIZE;

    // Copy bone matrices to mapped UBO (use currentFrame, not CurrentFrameIndex
    // which is the swapchain image index and can exceed MAX_FRAMES_IN_FLIGHT)
    memcpy(boneUBOMapped[currentFrame], data, size);

    // Bind bone descriptor set at set index 1
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLay, 1, 1,
                            &boneDescriptorSets[currentFrame],
                            0, nullptr);
}

    }
}
