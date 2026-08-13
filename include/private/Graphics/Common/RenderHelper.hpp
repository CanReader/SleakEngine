#ifndef _RENDERHELPER_HPP_
#define _RENDERHELPER_HPP_

#include <Core/OSDef.hpp>
#include <mutex> // For thread safety
#ifdef PLATFORM_WIN
#include <d3d11.h>    
#include <d3d12.h>    
#endif
#include <vulkan/vulkan.h>
#include <OpenGLRenderer.hpp> // You'll likely need to include headers for other renderers too

namespace Sleak {
namespace RenderEngine {

class RenderHelper {
    friend class DirectX12Renderer;
    friend class DirectX11Renderer;
    friend class VulkanRenderer;
    friend class OpenGLRenderer;

public:
    static void* GetDevice() {
        std::lock_guard<std::mutex> lock(deviceMutex);
        return Device;
    }

    static void* GetContext() {
        std::lock_guard<std::mutex> lock(contextMutex);
        return Context;
    }

#ifdef PLATFORM_WIN
    static ID3D11Device* GetD3D11Device() {
        std::lock_guard<std::mutex> lock(deviceMutex); // Thread safety
        return dynamic_cast<ID3D11Device*>(Device); // Type checking and casting
    }

    static ID3D11DeviceContext* GetD3D11DeviceContext() {
        std::lock_guard<std::mutex> lock(contextMutex); // Thread safety
        return dynamic_cast<ID3D11DeviceContext*>(Context); // Type checking and casting
    }

    static ID3D12Device* GetD3D12Device() {
        std::lock_guard<std::mutex> lock(deviceMutex); // Thread safety
        return dynamic_cast<ID3D12Device*>(Device); // Type checking and casting
    }

    static ID3D12GraphicsCommandList* GetD3D12CommandQueue() {
        std::lock_guard<std::mutex> lock(contextMutex); // Thread safety
        return dynamic_cast<ID3D12GraphicsCommandList*>(Context); // Type checking and casting
    }
#endif

    static VkDevice* GetVKDevice() {
        std::lock_guard<std::mutex> lock(deviceMutex); // Thread safety
        return dynamic_cast<VkDevice*>(Device); // Type checking and casting
    }

private:
    static void SetDevice(void* value) {
        std::lock_guard<std::mutex> lock(deviceMutex);
        Device = value;
    }

    static void SetContext(void* value) {
        std::lock_guard<std::mutex> lock(contextMutex);
         Context = value;
    }

    static void* Device;
    static void* Context;

    static std::mutex deviceMutex;
    static std::mutex contextMutex;
};

} // namespace RenderEngine
} // namespace Sleak

#endif // _RENDERHELPER_HPP_