#ifndef VULKANRENDERER_HPP
#define VULKANRENDERER_HPP

#include "../Common/Renderer.hpp"
#include "../Common/RenderContext.hpp"
#include "Graphics/Vulkan/VulkanShader.hpp"
#include "Graphics/Vulkan/VulkanTexture.hpp"
#include "Core/Logger.hpp"
#include <vulkan/vulkan.h>
#include "Graphics/Vulkan/VulkanBuffer.hpp"
#include <Runtime/Material.hpp>
#include <cstdint>
#include <vector>
#include <set>
#include <array>
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

namespace Sleak {
class ENGINE_API Window;
    namespace RenderEngine {

struct QueueIndices {
    uint32_t GraphicsIndex = UINT32_MAX;
    uint32_t ComputeIndex = UINT32_MAX;
    uint32_t TransferIndex = UINT32_MAX;
    uint32_t PresentIndex = UINT32_MAX;

    const float GraphicsPriority = 0.9f;
    const float ComputePriority = 0.8f;
    const float TransferPriority = 0.7f;
    const float PresentPriority = 1.0f;

    bool isComplete() {
        return GraphicsIndex != UINT32_MAX && PresentIndex != UINT32_MAX;
    }
};

struct SwapchainDetails {
    VkSurfaceCapabilitiesKHR caps;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class ENGINE_API VulkanRenderer : public Renderer, public RenderContext {
public:
    VulkanRenderer(Window* window);
    ~VulkanRenderer();

    virtual bool Initialize() override;
    virtual void BeginRender() override;
    virtual void EndRender() override;
    virtual void Cleanup() override;
    virtual void WaitIdle() override;
    virtual void FlushPendingTransfers() override;

    // GPU memory tracking
    virtual size_t GetGPUMemoryUsed() const override;
    virtual size_t GetGPUMemoryBudget() const override;

    virtual void Resize(uint32_t width, uint32_t height) override;

    inline void SetRender(bool value) { bRender = value; }
    inline bool GetRender() { return bRender; }

    /// Initializes ImGui and its Vulkan backend against the active render pass.
    virtual bool CreateImGUI() override;

    virtual RenderContext* GetContext() override { return this; }

    // Feature capability mask
    virtual uint32_t GetFeatureCaps() const override {
        return CapDeferred | CapSSAO | CapSSR | CapTAA | CapBloom | CapIBL |
               CapShadows | CapHDRTarget;
    }

    // RenderContext interface
    virtual void Draw(uint32_t vertexCount) override;
    virtual void DrawIndexed(uint32_t indexCount) override;
    virtual void DrawInstance(uint32_t instanceCount,
                              uint32_t vertexPerInstance) override;
    virtual void DrawIndexedInstance(uint32_t instanceCount,
                                     uint32_t indexPerInstance) override;

    virtual void SetRenderFace(RenderFace face) override;
    virtual void SetRenderMode(RenderMode mode) override;
    virtual void SetViewport(float x, float y, float width, float height,
                             float minDepth = 0.0f,
                             float maxDepth = 1.0f) override;
    virtual void ClearRenderTarget(float r, float g, float b,
                                   float a) override;
    virtual void ClearDepthStencil(bool clearDepth, bool clearStencil,
                                   float depth, uint8_t stencil) override;

    virtual void BindVertexBuffer(RefPtr<BufferBase> buffer,
                                  uint32_t slot = 0) override;
    virtual void BindIndexBuffer(RefPtr<BufferBase> buffer,
                                 uint32_t slot = 0) override;
    virtual void BindConstantBuffer(RefPtr<BufferBase> buffer,
                                    uint32_t slot = 0) override;

    virtual BufferBase* CreateBuffer(BufferType Type, uint32_t size,
                                     void* data) override;
    virtual Shader* CreateShader(const std::string& shaderSource) override;
    virtual Texture* CreateTexture(const std::string& TexturePath) override;
    virtual Texture* CreateTextureFromData(uint32_t width, uint32_t height,
                                           void* data) override;

    Texture* CreateCubemapTexture(const std::array<std::string, 6>& facePaths);
    Texture* CreateCubemapTextureFromPanorama(const std::string& panoramaPath);

    virtual void BindTexture(RefPtr<Sleak::Texture> texture, uint32_t slot = 0) override;
    virtual void BindTextureRaw(Sleak::Texture* texture, uint32_t slot = 0) override;
    /// Binds the skybox pipeline and its descriptor set for the current frame.
    virtual void BeginSkyboxPass() override;
    /// Restores the previous pipeline and descriptor set after the skybox draw.
    virtual void EndSkyboxPass() override;
    /// Copies bone matrices into the current frame's UBO and binds its descriptor set.
    virtual void BindBoneBuffer(RefPtr<BufferBase> buffer) override;
    /// Binds the skinned pipeline matching the currently active render pass.
    virtual void BeginSkinnedPass() override;
    /// Restores the previous pipeline after skinned draws.
    virtual void EndSkinnedPass() override;
    /// Binds the voxel pipeline matching the currently active render pass.
    virtual void BeginVoxelPass() override;
    /// Restores the previous pipeline and descriptor set after voxel draws.
    virtual void EndVoxelPass() override;
    /// Binds the debug line pipeline for the current frame.
    virtual void BeginDebugLinePass() override;
    /// Restores the previous pipeline and descriptor set after debug line draws.
    virtual void EndDebugLinePass() override;

    // Shadow pass support
    /// Marks the shadow pass active and invalidates the push constant cache.
    virtual void BeginShadowPass() override;
    /// Marks the shadow pass inactive.
    virtual void EndShadowPass() override;
    virtual bool IsShadowPassActive() const override { return m_shadowPassActive; }

    // Light UBO update (called by LightManager)
    /// Copies light and shadow data into the current frame's mapped UBO.
    void UpdateShadowLightUBO(const void* data, uint32_t size) override;
    /// Stages the light view-projection matrix for commit at the next BeginRender.
    void SetLightVP(const float* lightVP) override;

    // Deferred rendering overrides
    virtual bool IsDeferredEnabled() const override { return m_deferredEnabled && m_gbufferResourcesCreated; }
    virtual bool IsInGeometryPass() const override { return m_inGeometryPass; }
    /// Binds the GBuffer pipeline and marks the geometry pass active.
    /// Called by RenderCommandQueue when deferred is active.
    virtual void BindGBufferShader() override;
    /// Writes a material's textures and params into its ring slot and binds it at set 0.
    virtual void BindPBRMaterial(Sleak::Material* material) override;
    /// Runs the deferred lighting pass, reading the GBuffer and writing the HDR scene image.
    virtual void ExecuteDeferredLightingPass() override;
    /// Begins the forward transparent render pass over the HDR scene image.
    virtual void BeginForwardTransparentPass() override;
    /// Marks the forward transparent pass ended; EndRender closes the actual render pass.
    virtual void EndForwardTransparentPass() override;
    /// Copies deferred CB data into the current frame's UBO and snapshots the camera matrices.
    virtual void UpdateDeferredCB(const void* data, uint32_t size) override;

    // MSAA
    void ApplyMSAAChange() override;
    void ApplyVSyncChange() override;
    void ApplyShadowResolutionChange() override;

private:
    /// Compiles the skybox shaders and creates the skybox descriptor set and pipeline.
    bool CreateSkyboxPipeline();
    /// Compiles the skinned shaders and creates the forward skinned pipeline.
    bool CreateSkinnedPipeline();

    // Deferred rendering
    bool CreateGBufferResources();
    /// Creates the GBuffer render pass with its three color attachments and depth.
    bool CreateGBufferRenderPass();
    /// Creates the GBuffer framebuffer binding the GBuffer images and depth.
    bool CreateGBufferFramebuffer();
    /// Compiles the GBuffer shaders and creates the geometry pipeline, reusing pipelineLay.
    bool CreateGBufferPipeline();
    /// Compiles the skinned GBuffer shaders so skinned meshes write into the GBuffer.
    bool CreateSkinnedGbufferPipeline();
    /// Creates the deferred lighting render pass with a single color attachment.
    bool CreateLightingRenderPass();
    /// Creates one lighting pass framebuffer per swapchain image, all aliasing the HDR target.
    bool CreateLightingFramebuffers();
    /// Compiles the lighting shaders and creates the fullscreen lighting pipeline.
    bool CreateLightingPipeline();
    /// Creates the forward transparent render pass writing into the HDR scene image.
    bool CreateForwardRenderPass();
    /// Creates one forward transparent framebuffer per swapchain image, all aliasing the HDR target.
    bool CreateForwardFramebuffers();
    /// Creates the GBuffer sampler descriptor set layout, pool, and per-frame sets.
    bool CreateGBufferDescriptorSets();
    /// Creates the per-frame deferred constant buffer holding InvViewProj and screen size.
    bool CreateDeferredCBResources();
    /// Creates the PBR material descriptor layout, pool, ring of sets, and GBuffer geometry pipeline layout.
    bool CreatePBRMaterialResources();
    /// Creates stub IBL irradiance, prefilter, and BRDF LUT images, samplers, and descriptor set.
    bool CreateIBLResources();
    /// Destroys all GBuffer, lighting, and forward transparent pass resources.
    void CleanupGBufferResources();
    /// Destroys the IBL images, samplers, and descriptor resources.
    void CleanupIBLResources();
    /// Writes the GBuffer, depth, and shadow images into the sampler descriptor sets before the lighting pass.
    void UpdateGBufferDescriptors();

    // Shadow mapping
    /// Creates the shadow depth image, sampler, render pass, and framebuffer.
    bool CreateShadowResources();
    /// Compiles the shadow depth shader and creates the shadow pass pipeline.
    bool CreateShadowPipeline();
    /// Creates the per-frame light and shadow UBO buffers and descriptor sets.
    bool CreateShadowLightUBOResources();
    /// Destroys the shadow map image, pipeline, render pass, and light UBO resources.
    void CleanupShadowResources();
    /// Creates the Vulkan instance with validation layers when available.
    bool InitVulkan();
    /// Creates the SDL-backed Vulkan presentation surface.
    bool CreateSurface();
    /// Selects the physical GPU and creates the logical device and queues.
    bool CreateDevice();
    /// Creates the swapchain from the queried surface capabilities.
    bool CreateSwapChain();
    /// Rebuilds the swapchain and its dependents after resize or resolution change.
    bool RecreateSwapChain();
    /// Creates an image view for each swapchain image.
    bool CreateImageViews();
    /// Creates the main forward graphics pipeline and its pipeline layout.
    bool CreateGraphicsPipeline();
    /// Creates the main forward render pass with optional MSAA color and resolve attachments.
    bool CreateRenderPass();
    /// Creates one framebuffer per swapchain image for the main render pass.
    bool CreateFrameBuffer();
    bool CreateCommandPool();
    bool CreateCommandBuffer();
    bool CreateSyncObjects();
    /// Creates the depth image, memory, and image view.
    bool CreateDepthResources();
    /// Creates the texture, bone UBO, light UBO, and shadow sampler descriptor set layouts.
    bool CreateDescriptorSetLayout();
    /// Creates the descriptor pool backing the per-texture descriptor sets.
    bool CreateDescriptorPool();
    /// Allocates one texture descriptor set per swapchain image.
    bool AllocateDescriptorSets();
    /// Creates the fallback 1x1 white texture and writes it into the global descriptor sets.
    bool CreateDefaultTexture();
    /// Allocates and writes a per-texture descriptor set for the given texture.
    void WriteTextureDescriptors(VulkanTexture* texture);
    /// Registers the debug messenger callback for validation output.
    bool SetupDebugMessenger();

    /// Destroys the swapchain, its image views, and framebuffers.
    void CleanupSwapChain();
    /// Destroys the depth image, memory, and image view.
    void CleanupDepthResources();

    // MSAA resources
    /// Creates the MSAA color image used as the multisampled render target.
    bool CreateMSAAColorResources();
    /// Destroys the MSAA color image, view, and memory.
    void CleanupMSAAColorResources();
    /// Queries the highest MSAA sample count the GPU supports.
    VkSampleCountFlagBits GetMaxUsableSampleCount();

    virtual void ConfigureRenderMode() override;
    virtual void ConfigureRenderFace() override;

    /// Builds one queue create info per unique queue family index.
    std::vector<VkDeviceQueueCreateInfo>
    GetUniqueQueueCreateInfos();

    /// Queries surface capabilities, formats, and present modes.
    std::optional<SwapchainDetails> QuerySwapchain();

    /// Picks a UNORM surface format to avoid double sRGB encoding.
    VkSurfaceFormatKHR ChooseFormat(
        const std::vector<VkSurfaceFormatKHR>& formats);
    /// Picks FIFO when VSync is on, otherwise MAILBOX or IMMEDIATE.
    VkPresentModeKHR ChoosePresentMode(
        const std::vector<VkPresentModeKHR>& modes);
    /// Clamps the window size to the surface's supported extent.
    VkExtent2D ChooseExtend(SwapchainDetails details);

    /// Picks the first supported depth-stencil format from the candidate list.
    VkFormat FindDepthFormat();
    /// Finds a memory type index matching the filter and property flags.
    uint32_t FindMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);

    /// Fills the debug messenger create info with severity and callback.
    void PopulateDebugMessengerCreateInfo(
        VkDebugUtilsMessengerCreateInfoEXT& createInfo);

    /// Debug messenger callback that routes Vulkan messages to the logger.
    static VkBool32 Validation(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageTypes,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData);

    bool bRender = true;
    bool bFrameStarted = false;

    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
    uint32_t currentFrame = 0;
    uint32_t CurrentFrameIndex = 0;
    uint32_t m_semaphoreIndex = 0;  // cycles through swapchain image count
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkSwapchainKHR swapChain = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    VkCommandBuffer command = VK_NULL_HANDLE;  // alias for commandBuffers[currentFrame]
    std::vector<VkFramebuffer> swapChainFramebuffers;
    std::vector<VkImage> swapChainImages;
    std::vector<VkImageView> swapChainImageViews;
    VkFormat scImageFormat;
    VkExtent2D scExtent;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> GPUs;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLay = VK_NULL_HANDLE;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    std::vector<VkFence> imagesInFlight;
    VulkanShader* simpleShader = nullptr;
    VkClearValue clearColor;

    // Depth buffer
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;
    VkFormat depthFormat;

    // MSAA color buffer (multisample resolve target)
    VkSampleCountFlagBits m_msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    VkImage m_msaaColorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_msaaColorImageMemory = VK_NULL_HANDLE;
    VkImageView m_msaaColorImageView = VK_NULL_HANDLE;

    // Descriptor sets for uniform buffers
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets;

    QueueIndices QueueIDs;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    VkQueue transferQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT =
        nullptr;

    Window* sdlWindow;

    // Texture binding
    bool m_textureDescriptorsWritten = false;
    VulkanTexture* m_defaultTexture = nullptr;

    // Skybox pipeline
    VkPipeline skyboxPipeline = VK_NULL_HANDLE;
    VulkanShader* skyboxShader = nullptr;
    VkDescriptorPool skyboxDescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> skyboxDescriptorSets;
    bool m_skyboxDescriptorsWritten = false;
    VkImageView m_skyboxCubemapView = VK_NULL_HANDLE;   // cached for MSAA re-bind
    VkSampler m_skyboxCubemapSampler = VK_NULL_HANDLE;  // cached for MSAA re-bind

    // Skinned pipeline (uses skinned shaders with bone UBO)
    VkPipeline skinnedPipeline = VK_NULL_HANDLE;
    VulkanShader* skinnedShader = nullptr;

    // Debug line pipeline
    VkPipeline debugLinePipeline = VK_NULL_HANDLE;
    VulkanShader* debugLineShader = nullptr;
    /// Compiles the debug line shaders and creates the line-list pipeline.
    bool CreateDebugLinePipeline();

    // Voxel pipeline (compact 48-byte vertex layout for chunk meshes)
    VkPipeline m_voxelPipeline = VK_NULL_HANDLE;
    VkPipeline m_voxelShadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_gbufferVoxelPipeline = VK_NULL_HANDLE;
    bool m_inVoxelPass = false;
    /// Compiles the flat_shader SPIR-V and creates the forward and GBuffer voxel pipelines.
    bool CreateVoxelPipeline();
    /// Compiles the voxel shadow vertex shader and creates the voxel shadow-pass pipeline.
    bool CreateVoxelShadowPipeline();

    // Water pipeline (forward transparent, uses water_shader SPIR-V)
    VkPipeline m_waterPipeline = VK_NULL_HANDLE;
    VulkanShader* m_waterShader = nullptr;
    /// Compiles the optional water shaders and creates the alpha-blended water pipeline.
    bool CreateWaterPipeline();

    // Bone UBO (for skeletal animation — set 1, binding 0)
    VkDescriptorSetLayout boneDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool boneDescriptorPool = VK_NULL_HANDLE;
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> boneUBOBuffers = {};
    std::array<VkDeviceMemory, MAX_FRAMES_IN_FLIGHT> boneUBOMemory = {};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> boneUBOMapped = {};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> boneDescriptorSets = {};
    bool m_boneUBOCreated = false;
    /// Creates the per-frame bone UBO buffers and their descriptor sets.
    bool CreateBoneUBOResources();
    /// Destroys the bone UBO buffers, memory, and descriptor pool.
    void CleanupBoneUBOResources();

    // ImGUI
    VkDescriptorPool imguiDescriptorPool = VK_NULL_HANDLE;

    // Shadow mapping resources
    VkImage m_shadowImage = VK_NULL_HANDLE;
    VkDeviceMemory m_shadowImageMemory = VK_NULL_HANDLE;
    VkImageView m_shadowImageView = VK_NULL_HANDLE;
    VkSampler m_shadowSampler = VK_NULL_HANDLE;      // compare sampler (hardware PCF)
    VkSampler m_shadowRawSampler = VK_NULL_HANDLE;   // non-compare sampler (PCSS blocker search)
    VkRenderPass m_shadowRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_shadowFramebuffer = VK_NULL_HANDLE;
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    VulkanShader* m_shadowShader = nullptr;
    bool m_shadowPassActive = false;
    // m_shadowResourcesCreated lives in the Renderer base (shared with the
    // shadow-resolution change-request logic)

    // Shadow push-constant memo: LightVP*World is frame-constant per unique
    // World, so cache it and skip the matmul when consecutive casters (all
    // chunk draws share the identity transform) reuse the same World matrix.
    float m_shadowWorldCache[16] = {};
    float m_shadowPCCache[32] = {};
    bool  m_shadowPCCacheValid = false;

    // Light VP matrix (stored as raw floats for push constant computation)
    float m_lightVP[16] = {};
    // Staging slot: SetLightVP writes here; BeginRender copies it to
    // m_lightVP before the shadow pass. Keeps m_lightVP stable for the
    // entire frame so shadow pass and main pass agree on the transform.
    float m_pendingLightVP[16] = {};
    bool  m_hasPendingLightVP = false;

    // Light/Shadow UBO (set 2, binding 0)
    VkDescriptorSetLayout m_lightUBODescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_lightUBODescriptorPool = VK_NULL_HANDLE;
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> m_lightUBOBuffers = {};
    std::array<VkDeviceMemory, MAX_FRAMES_IN_FLIGHT> m_lightUBOMemory = {};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> m_lightUBOMapped = {};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_lightUBODescriptorSets = {};

    // Shadow map sampler descriptor (set 3, binding 0)
    VkDescriptorSetLayout m_shadowSamplerDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_shadowSamplerDescriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_shadowSamplerDescriptorSets = {};
    bool m_lightUBOCreated = false;

    // Async buffer transfer (zero-CPU-blocking GPU uploads)
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> m_transferSemaphores = {};
    std::array<VulkanBuffer::AsyncFlushResult, MAX_FRAMES_IN_FLIGHT> m_asyncFlush;

    // ---- Deferred GBuffer ----
    // 3 color attachments: RT0=AlbedoAO, RT1=NormalRough, RT2=MetalEmit.
    // World position is reconstructed from the depth buffer + InvViewProj in
    // the lighting/SSAO/SSR passes (no RGBA32F worldpos RT) to cut GBuffer
    // bandwidth on a fill-bound renderer.
    static constexpr uint32_t GBUFFER_COUNT = 3;
    VkImage        m_gbufferImages[GBUFFER_COUNT]   = {};
    VkDeviceMemory m_gbufferMemory[GBUFFER_COUNT]   = {};
    VkImageView    m_gbufferViews[GBUFFER_COUNT]    = {};
    /// GBuffer attachment formats: RT0 AlbedoAO, RT1 NormalRough, RT2 MetalEmit. Defined in VulkanDeferred.cpp.
    static const VkFormat m_gbufferFormats[GBUFFER_COUNT];
    VkRenderPass   m_gbufferRenderPass              = VK_NULL_HANDLE;
    VkFramebuffer  m_gbufferFramebuffer             = VK_NULL_HANDLE;
    VkPipeline     m_gbufferPipeline                = VK_NULL_HANDLE;
    VkPipeline     m_skinnedGbufferPipeline         = VK_NULL_HANDLE;
    VkPipelineLayout m_gbufferPipelineLayout        = VK_NULL_HANDLE;
    VulkanShader*  m_gbufferShader                  = nullptr;
    bool           m_gbufferResourcesCreated        = false;
    bool           m_inGeometryPass                 = false;

    // Lighting pass
    VkRenderPass   m_lightingRenderPass             = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_lightingFramebuffers;   // one per swapchain image
    VkPipeline     m_lightingPipeline               = VK_NULL_HANDLE;
    VkPipelineLayout m_lightingPipelineLayout       = VK_NULL_HANDLE;
    VulkanShader*  m_lightingShader                 = nullptr;

    // Forward transparent pass
    VkRenderPass   m_forwardRenderPass              = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_forwardFramebuffers;  // one per swapchain image (color+depth, non-MSAA)
    bool           m_inForwardTransparentPass       = false;
    bool           m_forwardPassOpen                = false; // true when forward RP is currently recording

    // GBuffer sampler descriptor set (set 0 in lighting pass)
    VkDescriptorSetLayout m_gbufferSamplerDSL       = VK_NULL_HANDLE;
    VkDescriptorPool      m_gbufferSamplerPool      = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_gbufferSamplerSets = {};
    VkSampler      m_gbufferSampler                 = VK_NULL_HANDLE;
    VkSampler      m_depthSampler                   = VK_NULL_HANDLE;

    // Deferred CB UBO (set 1 in lighting pass): InvViewProj + screenSize + near/far
    struct DeferredCBData {
        float InvViewProj[16];
        float ScreenW, ScreenH, NearP, FarP;
    };
    VkDescriptorSetLayout m_deferredCBDSL           = VK_NULL_HANDLE;
    VkDescriptorPool      m_deferredCBPool          = VK_NULL_HANDLE;
    std::array<VkBuffer,       MAX_FRAMES_IN_FLIGHT> m_deferredCBBuffers = {};
    std::array<VkDeviceMemory, MAX_FRAMES_IN_FLIGHT> m_deferredCBMemory  = {};
    std::array<void*,          MAX_FRAMES_IN_FLIGHT> m_deferredCBMapped  = {};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_deferredCBSets   = {};
    bool m_deferredCBCreated = false;

    // PBR material descriptor resources (GBuffer geometry pass, set 0)
    // One per-frame descriptor set updated with each BindPBRMaterial() call.
    struct alignas(16) PBRMaterialParams {
        float albedoFactorR, albedoFactorG, albedoFactorB, albedoFactorA; // vec4
        float metallicFactor, roughnessFactor, aoFactor, normalIntensity;  // 4 floats
        float emissiveR, emissiveG, emissiveB, emissiveIntensity;          // vec4
        float tilingX, tilingY, offsetX, offsetY;                          // 4 floats
        uint32_t hasNormalMap, hasMetallicMap, hasRoughnessMap, hasAOMap;  // 4 uints
        uint32_t hasEmissiveMap; float _pad0, _pad1, _pad2;               // 4 floats
    };
    // Per-frame RING of PBR material sets: each material drawn in a frame gets
    // its own set + its own UBO sub-region, so a set/region is never rewritten
    // while already bound in the recording command buffer (UPDATE_AFTER_BIND VUID).
    static constexpr uint32_t PBR_SETS_PER_FRAME = 64;
    static constexpr uint32_t PBR_SET_COUNT = MAX_FRAMES_IN_FLIGHT * PBR_SETS_PER_FRAME;
    VkDescriptorSetLayout m_pbrMaterialDSL         = VK_NULL_HANDLE;
    VkDescriptorPool      m_pbrMaterialPool        = VK_NULL_HANDLE;
    // One params UBO per frame, sub-addressed by slot at m_pbrMaterialUBOStride.
    std::array<VkBuffer,        MAX_FRAMES_IN_FLIGHT> m_pbrMaterialCBBuffers = {};
    std::array<VkDeviceMemory,  MAX_FRAMES_IN_FLIGHT> m_pbrMaterialCBMemory  = {};
    std::array<void*,           MAX_FRAMES_IN_FLIGHT> m_pbrMaterialCBMapped  = {};
    std::array<VkDescriptorSet, PBR_SET_COUNT>        m_pbrMaterialSets      = {};
    VkDeviceSize m_pbrMaterialUBOStride = 0;
    uint32_t m_pbrMaterialSlot[MAX_FRAMES_IN_FLIGHT] = {};
    bool m_pbrMaterialResourcesCreated = false;
    VkPipelineLayout m_gbufferGeomLayout = VK_NULL_HANDLE;

    // IBL resources (lighting pass, set 3)
    VkImage        m_iblIrradianceImage   = VK_NULL_HANDLE;
    VkDeviceMemory m_iblIrradianceMemory  = VK_NULL_HANDLE;
    VkImageView    m_iblIrradianceView    = VK_NULL_HANDLE;
    VkSampler      m_iblIrradianceSampler = VK_NULL_HANDLE;

    VkImage        m_iblPrefilterImage    = VK_NULL_HANDLE;
    VkDeviceMemory m_iblPrefilterMemory   = VK_NULL_HANDLE;
    VkImageView    m_iblPrefilterView     = VK_NULL_HANDLE;
    VkSampler      m_iblPrefilterSampler  = VK_NULL_HANDLE;
    static constexpr uint32_t IBL_PREFILTER_MIP_LEVELS = 5;

    VkImage        m_iblBrdfLutImage      = VK_NULL_HANDLE;
    VkDeviceMemory m_iblBrdfLutMemory     = VK_NULL_HANDLE;
    VkImageView    m_iblBrdfLutView       = VK_NULL_HANDLE;
    VkSampler      m_iblBrdfLutSampler    = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_iblDSL        = VK_NULL_HANDLE;
    VkDescriptorPool      m_iblPool       = VK_NULL_HANDLE;
    std::array<VkBuffer,        MAX_FRAMES_IN_FLIGHT> m_iblSettingsBuffers = {};
    std::array<VkDeviceMemory,  MAX_FRAMES_IN_FLIGHT> m_iblSettingsMemory  = {};
    std::array<void*,           MAX_FRAMES_IN_FLIGHT> m_iblSettingsMapped  = {};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_iblSets            = {};
    bool m_iblResourcesCreated = false;
    bool m_iblReady            = false;

    // ---- SSAO resources ----
    // Half-resolution R8 occlusion buffer (raw SSAO + blurred).
    static constexpr uint32_t SSAO_KERNEL_SIZE = 32;
    static constexpr uint32_t SSAO_NOISE_SIZE  = 4;
    bool       m_ssaoResourcesCreated      = false;
    VkExtent2D m_ssaoExtent                = {0, 0};
    VkFormat   m_ssaoFormat                = VK_FORMAT_R8_UNORM;

    VkImage        m_ssaoRawImage          = VK_NULL_HANDLE;
    VkDeviceMemory m_ssaoRawMemory         = VK_NULL_HANDLE;
    VkImageView    m_ssaoRawView           = VK_NULL_HANDLE;
    VkFramebuffer  m_ssaoRawFramebuffer    = VK_NULL_HANDLE;

    VkImage        m_ssaoBlurImage         = VK_NULL_HANDLE;
    VkDeviceMemory m_ssaoBlurMemory        = VK_NULL_HANDLE;
    VkImageView    m_ssaoBlurView          = VK_NULL_HANDLE;
    VkFramebuffer  m_ssaoBlurFramebuffer   = VK_NULL_HANDLE;

    VkRenderPass   m_ssaoRenderPass        = VK_NULL_HANDLE;  // shared for raw + blur
    VkPipeline     m_ssaoPipeline          = VK_NULL_HANDLE;
    VkPipeline     m_ssaoBlurPipeline      = VK_NULL_HANDLE;
    VkPipelineLayout m_ssaoPipelineLayout  = VK_NULL_HANDLE;
    VkPipelineLayout m_ssaoBlurPipelineLayout = VK_NULL_HANDLE;
    VulkanShader*  m_ssaoShader            = nullptr;
    VulkanShader*  m_ssaoBlurShader        = nullptr;

    VkSampler      m_ssaoSampler           = VK_NULL_HANDLE;  // linear clamp
    VkSampler      m_ssaoPointSampler      = VK_NULL_HANDLE;  // nearest clamp for depth

    // SSAO noise texture (4x4 RGBA random rotation vectors)
    VkImage        m_ssaoNoiseImage        = VK_NULL_HANDLE;
    VkDeviceMemory m_ssaoNoiseMemory       = VK_NULL_HANDLE;
    VkImageView    m_ssaoNoiseView         = VK_NULL_HANDLE;
    VkSampler      m_ssaoNoiseSampler      = VK_NULL_HANDLE;

    // SSAO pass descriptor sets
    struct alignas(16) SSAOParams {
        float View[16];
        float Projection[16];
        float InvViewProj[16];              // inverse(View*Proj) for depth recon
        float Kernel[SSAO_KERNEL_SIZE][4];  // xyz=dir, w=pad
        float ScreenW, ScreenH;
        float NoiseScaleX, NoiseScaleY;
        float Radius, Bias, Power, Intensity;
        uint32_t KernelSize;
        float _pad0, _pad1, _pad2;
    };
    VkDescriptorSetLayout m_ssaoInputDSL       = VK_NULL_HANDLE;  // set 0: samplers
    VkDescriptorSetLayout m_ssaoUboDSL         = VK_NULL_HANDLE;  // set 1: UBO
    VkDescriptorSetLayout m_ssaoBlurDSL        = VK_NULL_HANDLE;  // set 0: blur input + depth
    VkDescriptorPool      m_ssaoDescriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_ssaoInputSets = {};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_ssaoUboSets   = {};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_ssaoBlurSets  = {};
    std::array<VkBuffer,       MAX_FRAMES_IN_FLIGHT>  m_ssaoUboBuffers = {};
    std::array<VkDeviceMemory, MAX_FRAMES_IN_FLIGHT>  m_ssaoUboMemory  = {};
    std::array<void*,          MAX_FRAMES_IN_FLIGHT>  m_ssaoUboMapped  = {};

    // Cached camera state for SSAO UBO fill (set from SetViewProj or inferred
    // from DeferredCB's InvViewProj). We populate View+Projection at SSAO time.
    float m_cachedView[16]       = {};
    float m_cachedProjection[16] = {};
    // inverse(View*Proj) snapshot from the lighting DeferredCB — reused by the
    // SSAO/SSR passes to reconstruct world position from the depth buffer.
    float m_cachedInvViewProj[16] = {};

    /// Creates all SSAO images, render pass, framebuffers, descriptors, and pipelines.
    bool CreateSSAOResources();
    // One-time init of the disabled-effect fallback images (ssaoBlur=white,
    // ssr=black, bloom mip0=black) to SHADER_READ_ONLY so the per-frame
    // disabled paths can skip re-clearing them every frame.
    /// Clears the SSAO/SSR/bloom fallback images once so disabled effects sample defined black/white content.
    void InitDisabledEffectFallbacks();
    /// Destroys all SSAO pipelines, framebuffers, descriptors, images, and samplers.
    void CleanupSSAOResources();
    /// Creates the full-res raw and blurred SSAO color images, views, and samplers.
    bool CreateSSAOImages();
    /// Creates the shared SSAO render pass (R8 color, DONT_CARE load, shader-read-only output).
    bool CreateSSAORenderPass();
    /// Creates the raw and blur SSAO framebuffers.
    bool CreateSSAOFramebuffers();
    /// Compiles the SSAO and SSAO-blur shaders and creates their pipelines.
    bool CreateSSAOPipelines();
    /// Creates the SSAO input/UBO/blur descriptor layouts, pool, sets, and UBO buffers.
    bool CreateSSAODescriptorResources();
    /// Generates and uploads the 4x4 tangent-plane rotation noise texture.
    bool CreateSSAONoiseTexture();
    /// Writes the GBuffer, depth, and noise samplers into the SSAO input and blur descriptor sets.
    void UpdateSSAODescriptors();
    /// Fills the SSAO UBO with the cached camera matrices and a cosine-weighted hemisphere kernel.
    void UpdateSSAOUBO();
    /// Runs the raw SSAO and bilateral blur passes, or clears the blur target when SSAO is disabled.
    void RenderSSAOPasses();

    // ---- SSR (Screen-Space Reflections) resources ----
    // Full-resolution R16G16B16A16 premultiplied reflection buffer. Rendered
    // after the forward pass, added into the HDR scene by the bloom composite.
    struct alignas(16) SSRParams {
        float View[16];
        float Projection[16];
        float InvViewProj[16];      // inverse(View*Proj) for depth recon
        float CameraPos[4];         // xyz = world pos, w = pad
        float ScreenW, ScreenH;
        float MaxDistance;          // view-space ray march distance
        float Thickness;            // depth intersection thickness
        int   NumSteps;             // coarse steps
        int   NumBinarySteps;       // refinement steps
        float RoughnessThreshold;   // beyond this, no reflections
        float _pad;
    };

    bool       m_ssrResourcesCreated = false;
    // m_ssrEnabled is inherited from RenderEngine::Renderer (base class).
    VkFormat   m_ssrFormat           = VK_FORMAT_R16G16B16A16_SFLOAT;

    VkImage        m_ssrImage         = VK_NULL_HANDLE;
    VkDeviceMemory m_ssrMemory        = VK_NULL_HANDLE;
    VkImageView    m_ssrView          = VK_NULL_HANDLE;
    VkSampler      m_ssrSampler       = VK_NULL_HANDLE;
    VkRenderPass   m_ssrRenderPass    = VK_NULL_HANDLE;
    VkFramebuffer  m_ssrFramebuffer   = VK_NULL_HANDLE;
    VkPipeline     m_ssrPipeline      = VK_NULL_HANDLE;
    VkPipelineLayout m_ssrPipelineLayout = VK_NULL_HANDLE;
    VulkanShader*  m_ssrShader        = nullptr;

    VkDescriptorSetLayout m_ssrInputDSL = VK_NULL_HANDLE;  // set 0: 6 samplers
    VkDescriptorSetLayout m_ssrUboDSL   = VK_NULL_HANDLE;  // set 1: UBO
    VkDescriptorPool      m_ssrPool    = VK_NULL_HANDLE;

    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_ssrInputSets{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_ssrUboSets{};
    std::array<VkBuffer,        MAX_FRAMES_IN_FLIGHT> m_ssrUboBuffers{};
    std::array<VkDeviceMemory,  MAX_FRAMES_IN_FLIGHT> m_ssrUboMemory{};
    std::array<void*,           MAX_FRAMES_IN_FLIGHT> m_ssrUboMapped{};

    /// Creates the SSR image, render pass, framebuffer, descriptors, UBOs, and pipeline.
    bool CreateSSRResources();
    /// Destroys the SSR pipeline, framebuffer, descriptors, image, and sampler.
    void CleanupSSRResources();
    /// Fills the SSR UBO with the cached camera matrices, camera position, and ray march parameters.
    void UpdateSSRUBO();
    /// Writes the GBuffer and HDR scene samplers into the SSR input descriptor sets.
    void UpdateSSRDescriptors();
    /// Ray marches screen-space reflections into the SSR buffer, or clears it when SSR is disabled.
    void RenderSSRPass();

    // ---- TAA (Temporal Anti-Aliasing) resources ----
    // Ping-pong history accumulation with depth-based reprojection
    // and 3x3 neighborhood AABB clamping.
    struct alignas(16) TAAParams {
        float InvCurrentVP[16]; // inverse of unjittered current VP
        float PrevVP[16];       // previous frame unjittered VP
        float ScreenW, ScreenH;
        float BlendFactor;
        float _pad;
    };

    bool     m_taaResourcesCreated = false;
    // m_taaEnabled is inherited from RenderEngine::Renderer (base class).
    uint64_t m_taaFrameIdx         = 0;   // global counter; ping-pong = idx % 2

    VkImage        m_taaImages[2]     = {};
    VkDeviceMemory m_taaMemory[2]     = {};
    VkImageView    m_taaViews[2]      = {};
    VkRenderPass   m_taaRenderPass    = VK_NULL_HANDLE;
    VkFramebuffer  m_taaFramebufs[2]  = {};
    VkSampler      m_taaSampler       = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_taaInputDSL = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_taaUboDSL   = VK_NULL_HANDLE;
    VkDescriptorPool      m_taaPool     = VK_NULL_HANDLE;

    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_taaInputSets{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_taaUboSets{};
    std::array<VkBuffer,        MAX_FRAMES_IN_FLIGHT> m_taaUboBuffers{};
    std::array<VkDeviceMemory,  MAX_FRAMES_IN_FLIGHT> m_taaUboMemory{};
    std::array<void*,           MAX_FRAMES_IN_FLIGHT> m_taaUboMapped{};

    VkPipelineLayout m_taaPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_taaPipeline       = VK_NULL_HANDLE;
    VulkanShader*    m_taaShader         = nullptr;

    float m_prevViewProj[16] = {};  // previous frame unjittered VP (row-major)
    float m_taaJitter[2]     = {};  // current frame jitter in UV space

    /// Creates the ping-pong TAA history images, render pass, framebuffers, descriptors, and pipeline.
    bool CreateTAAResources();
    /// Destroys the TAA pipeline, framebuffers, descriptors, images, and sampler.
    void CleanupTAAResources();
    /// Computes the inverse current view-projection and reprojection blend factor into the TAA UBO.
    void UpdateTAAUBO();
    /// Resolves the current frame against TAA history and copies the result back into the HDR scene image.
    void RenderTAAPass();

    // Per-image "fallback content is valid" flags. Set true once the image is
    // primed (static black/white in SHADER_READ_ONLY) by either
    // InitDisabledEffectFallbacks or a disabled-path clear; set false whenever
    // the enabled path renders into the image (dirtying it). When true, the
    // disabled path skips its redundant per-frame clear. This stays correct
    // across runtime enable->disable toggles: the first disabled frame after a
    // toggle re-primes, then subsequent disabled frames skip.
    bool m_ssaoFallbackPrimed  = false;
    bool m_ssrFallbackPrimed   = false;
    bool m_bloomFallbackPrimed = false;

    // ---- Bloom + HDR post-processing resources ----
    // We render the lighting pass into an HDR scene-color image (not the
    // swapchain). Then we run the bloom pyramid and a final composite that
    // tonemaps + combines bloom into the swapchain.
    static constexpr uint32_t BLOOM_MIP_COUNT = 6;
    bool       m_bloomResourcesCreated     = false;
    VkFormat   m_hdrSceneFormat            = VK_FORMAT_R16G16B16A16_SFLOAT;

    // HDR scene color image (lighting pass target)
    VkImage        m_hdrSceneImage         = VK_NULL_HANDLE;
    VkDeviceMemory m_hdrSceneMemory        = VK_NULL_HANDLE;
    VkImageView    m_hdrSceneView          = VK_NULL_HANDLE;
    VkFramebuffer  m_hdrSceneFramebuffer   = VK_NULL_HANDLE;
    VkRenderPass   m_hdrLightingRenderPass = VK_NULL_HANDLE;

    // Bloom mip chain — single image with BLOOM_MIP_COUNT mip levels; each
    // level gets its own VkImageView so we can render into/out of it.
    VkImage        m_bloomImage            = VK_NULL_HANDLE;
    VkDeviceMemory m_bloomMemory           = VK_NULL_HANDLE;
    std::array<VkImageView, BLOOM_MIP_COUNT> m_bloomMipViews = {};
    std::array<VkFramebuffer, BLOOM_MIP_COUNT> m_bloomMipFramebuffers = {};
    std::array<VkExtent2D, BLOOM_MIP_COUNT> m_bloomMipExtents = {};

    VkRenderPass   m_bloomRenderPass       = VK_NULL_HANDLE;   // shared, loadOp=DONT_CARE, color output
    VkRenderPass   m_bloomAddRenderPass    = VK_NULL_HANDLE;   // LOAD, blend-add
    VkRenderPass   m_bloomCompositeRenderPass = VK_NULL_HANDLE; // swapchain target (DONT_CARE -> PRESENT_SRC)
    std::vector<VkFramebuffer> m_bloomCompositeFramebuffers;   // one per swapchain image

    VkPipeline     m_bloomThresholdPipeline = VK_NULL_HANDLE;
    VkPipeline     m_bloomDownsamplePipeline = VK_NULL_HANDLE;
    VkPipeline     m_bloomUpsamplePipeline = VK_NULL_HANDLE;
    VkPipeline     m_bloomCompositePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomFilterPipelineLayout = VK_NULL_HANDLE;  // src-only + push constant
    VkPipelineLayout m_bloomCompositePipelineLayout = VK_NULL_HANDLE; // two inputs + push constant
    VulkanShader*  m_bloomThresholdShader  = nullptr;
    VulkanShader*  m_bloomDownsampleShader = nullptr;
    VulkanShader*  m_bloomUpsampleShader   = nullptr;
    VulkanShader*  m_bloomCompositeShader  = nullptr;

    // One descriptor set per bloom transition (threshold + BLOOM_MIP_COUNT-1
    // downsamples + BLOOM_MIP_COUNT-1 upsamples) per frame slot.
    // Simpler: allocate a pool with enough sets for all transitions and
    // re-write them each frame.
    VkDescriptorSetLayout m_bloomFilterDSL    = VK_NULL_HANDLE;  // 1 sampler
    VkDescriptorSetLayout m_bloomCompositeDSL = VK_NULL_HANDLE;  // 2 samplers
    VkDescriptorPool      m_bloomDescriptorPool = VK_NULL_HANDLE;

    // Pre-allocated descriptor sets (per frame slot, per transition)
    //   transitions = 1 (threshold -> mip0) + (BLOOM_MIP_COUNT-1) downsample
    //               + (BLOOM_MIP_COUNT-1) upsample
    static constexpr uint32_t BLOOM_TRANSITION_COUNT = 1 + (BLOOM_MIP_COUNT - 1) + (BLOOM_MIP_COUNT - 1);
    std::array<std::array<VkDescriptorSet, BLOOM_TRANSITION_COUNT>, MAX_FRAMES_IN_FLIGHT> m_bloomFilterSets = {};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_bloomCompositeSets = {};

    VkSampler m_bloomSampler = VK_NULL_HANDLE;  // linear clamp

    /// Creates the HDR scene, bloom mip chain, passes, descriptors, pipelines, and TAA resources.
    bool CreateBloomResources();
    /// Destroys all bloom and HDR scene resources, then cleans up TAA resources.
    void CleanupBloomResources();
    /// Creates the HDR scene color image, view, and the shared bloom-source sampler.
    bool CreateHDRSceneResources();
    /// Creates the multi-mip bloom image and a per-mip image view.
    bool CreateBloomImages();
    /// Creates the bloom threshold/downsample, additive-upsample, and composite render passes.
    bool CreateBloomRenderPasses();
    /// Creates the per-mip bloom framebuffers and one composite framebuffer per swapchain image.
    bool CreateBloomFramebuffers();
    /// Compiles the bloom threshold/downsample/upsample/composite shaders and creates their pipelines.
    bool CreateBloomPipelines();
    /// Creates the bloom filter and composite descriptor layouts, pool, and sets.
    bool CreateBloomDescriptorResources();
    /// Runs the threshold, downsample, and additive-upsample bloom mip chain, or clears mip 0 when bloom is disabled.
    void RenderBloomPass();
    /// Tonemaps and composites the HDR scene, bloom, and SSR into the swapchain image, then draws ImGui.
    void RenderBloomCompositePass();

    // Helper used by the upload path of the default renderer to pick a
    // reasonable linear-clamp sampler for post-process work.
    /// Sets the dynamic viewport and scissor to fill the given extent. Defined in VulkanBloom.cpp (most call sites of the four post-effect TUs).
    static void FillFullscreenViewportScissor(VkCommandBuffer cmd, VkExtent2D ext);
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // VULKANRENDERER_HPP
