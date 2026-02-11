#ifndef RENDERERFACTORY_HPP
#define RENDERERFACTORY_HPP

#include "Renderer.hpp"
#include <Graphics/Vulkan/VulkanRenderer.hpp>
#include <Graphics/OpenGL/OpenGLRenderer.hpp>
#include <Graphics/DirectX/DirectX11Renderer.hpp>
#include <Graphics/DirectX/DirectX12Renderer.hpp>
#include <Window.hpp>
#include <memory>

#include <Core/OSDef.hpp>

namespace Sleak {
    namespace RenderEngine {

class ENGINE_API RendererFactory {
public:
    static Renderer* CreateRenderer(RendererType type, Window* window);
    static Renderer* ParseArg(std::string arg, Window* window);
    inline static RendererType GetRendererType() {return Renderertype;}

private:
    static RendererType Renderertype;
};

}
}

#endif // RENDERERFACTORY_HPP
