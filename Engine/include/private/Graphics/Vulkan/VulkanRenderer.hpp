#ifndef VULKANRENDERER_HPP
#define VULKANRENDERER_HPP

#include "../Renderer.hpp"
#include "../RenderContext.hpp"
#include "Graphics/Vulkan/VulkanShader.hpp"
#include "Graphics/Vulkan/VulkanTexture.hpp"
#include "Logger.hpp"
#include <vulkan/vulkan.h>
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

    virtual void Resize(uint32_t width, uint32_t height) override;

    inline void SetRender(bool value) { bRender = value; }
    inline bool GetRender() { return bRender; }

    virtual bool CreateImGUI() override;

    virtual RenderContext* GetContext() override { return this; }

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
    virtual void BeginSkyboxPass() override;
    virtual void EndSkyboxPass() override;
    virtual void BindBoneBuffer(RefPtr<BufferBase> buffer) override;
    virtual void BeginSkinnedPass() override;
    virtual void EndSkinnedPass() override;
    virtual void BeginDebugLinePass() override;
    virtual void EndDebugLinePass() override;

private:
    bool CreateSkyboxPipeline();
    bool CreateSkinnedPipeline();
    void UpdateSkyboxDescriptorSets();
    bool InitVulkan();
    bool CreateSurface();
    bool CreateDevice();
    bool CreateSwapChain();
    bool RecreateSwapChain();
    bool CreateImageViews();
    bool CreateGraphicsPipeline();
    bool CreateRenderPass();
    bool CreateFrameBuffer();
    bool CreateCommandPool();
    bool CreateCommandBuffer();
    bool CreateSyncObjects();
    bool CreateDepthResources();
    bool CreateDescriptorSetLayout();
    bool CreateDescriptorPool();
    bool AllocateDescriptorSets();
    bool CreateDefaultTexture();
    void WriteTextureDescriptors(VulkanTexture* texture);
    bool SetupDebugMessenger();

    void CleanupSwapChain();
    void CleanupDepthResources();

    virtual void ConfigureRenderMode() override;
    virtual void ConfigureRenderFace() override;

    std::vector<VkDeviceQueueCreateInfo>
    GetUniqueQueueCreateInfos();

    std::optional<SwapchainDetails> QuerySwapchain();

    VkSurfaceFormatKHR ChooseFormat(
        const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR ChoosePresentMode(
        const std::vector<VkPresentModeKHR>& modes);
    VkExtent2D ChooseExtend(SwapchainDetails details);

    VkFormat FindDepthFormat();
    uint32_t FindMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);

    void PopulateDebugMessengerCreateInfo(
        VkDebugUtilsMessengerCreateInfoEXT& createInfo);

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
    VulkanShader* simpleShader = nullptr;
    VkClearValue clearColor;

    // Depth buffer
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;
    VkFormat depthFormat;

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

    // Skinned pipeline (uses skinned shaders with bone UBO)
    VkPipeline skinnedPipeline = VK_NULL_HANDLE;
    VulkanShader* skinnedShader = nullptr;

    // Debug line pipeline
    VkPipeline debugLinePipeline = VK_NULL_HANDLE;
    VulkanShader* debugLineShader = nullptr;
    bool CreateDebugLinePipeline();

    // Bone UBO (for skeletal animation — set 1, binding 0)
    VkDescriptorSetLayout boneDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool boneDescriptorPool = VK_NULL_HANDLE;
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> boneUBOBuffers = {};
    std::array<VkDeviceMemory, MAX_FRAMES_IN_FLIGHT> boneUBOMemory = {};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> boneUBOMapped = {};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> boneDescriptorSets = {};
    bool m_boneUBOCreated = false;
    bool CreateBoneUBOResources();
    void CleanupBoneUBOResources();

    // ImGUI
    VkDescriptorPool imguiDescriptorPool = VK_NULL_HANDLE;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // VULKANRENDERER_HPP
