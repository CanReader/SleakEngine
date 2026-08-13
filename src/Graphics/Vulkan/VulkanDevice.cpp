#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "../../include/private/Graphics/Vulkan/VulkanBuffer.hpp"

#include <SDL3/SDL_vulkan.h>
#include <Core/Window.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "Core/Logger.hpp"
#include "SDL3/SDL_error.h"
#ifdef PLATFORM_LINUX
    #include "vulkan/vulkan_wayland.h"
#elif defined(PLATFORM_WIN)
    #include <vulkan/vulkan_win32.h>
#endif

namespace Sleak {
    namespace RenderEngine {

/// Creates the Vulkan instance with validation layers when available.
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

/// Selects the physical GPU and creates the logical device and queues.
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

/// Builds one queue create info per unique queue family index.
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

/// Registers the debug messenger callback for validation output.
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

/// Creates the SDL-backed Vulkan presentation surface.
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

/// Fills the debug messenger create info with severity and callback.
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

/// Debug messenger callback that routes Vulkan messages to the logger.
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

}
}
