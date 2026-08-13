#include "../../include/private/Graphics/RendererFactory.hpp"
#include <cstring>
#include <string>
#include <memory>
#include <stdexcept>
#include "Logger.hpp"

namespace Sleak {
namespace RenderEngine {

// Define the static member variable
RendererType RendererFactory::Renderertype = RendererType::Vulkan;

Renderer* RendererFactory::CreateRenderer(RendererType type, Window* window) {
    Renderertype = type;
    switch (type) {
        case RendererType::Vulkan:
            SLEAK_INFO("Used graphic API is Vulkan");
            #define API_VULKAN
            return new VulkanRenderer(window);
        case RendererType::OpenGL:
            SLEAK_INFO("Used graphic API is OpenGL");
            #define API_OPENGL
            return new OpenGLRenderer(window);
        #ifdef PLATFORM_WIN
        case RendererType::DirectX11:
            SLEAK_INFO("Used graphic API is DirectX 11");
            #define API_DIRECTX11
            return new DirectX11Renderer(window);
        case RendererType::DirectX12:
            if(DirectX12Renderer::IsSupport()) {
                SLEAK_INFO("DirectX 12 is supported in this device");
                #define API_DIRECTX12
                return new DirectX12Renderer(window);
            }
            else {
                SLEAK_WARN("The GPU does not support DirectX 12, using DirectX 11 instead");
                return CreateRenderer(RendererType::DirectX11, window);
            }
        #else
            throw std::runtime_error("DirectX is supported on windows!");
        #endif
        default:
            throw std::runtime_error("Unsupported renderer type");
    }
}

Renderer* RendererFactory::ParseArg(std::string arg, Window* window) {
    SLEAK_INFO("Passed renderer argument: {0}",arg)

    if(arg == "v" || arg == "vulkan")
        return CreateRenderer(RendererType::Vulkan, window);
    else if(arg == "o" || arg == "opengl")
        return CreateRenderer(RendererType::OpenGL, window);
    else if(arg == "d11" || arg == "d3d11" || arg == "directx11")
        return CreateRenderer(RendererType::DirectX11, window);
        else if(arg == "d12" || arg == "d3d12" || arg == "directx12")
        return CreateRenderer(RendererType::DirectX12, window);
    else
        throw std::runtime_error("Unsupported API has been requested!");
}

} // namespace RenderEngine
} // namespace Sleak
