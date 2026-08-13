#ifndef RENDERERFACTORY_HPP
#define RENDERERFACTORY_HPP

#include "Renderer.hpp"
#include <Graphics/Vulkan/VulkanRenderer.hpp>
#include <Graphics/OpenGL/OpenGLRenderer.hpp>
#include <Graphics/DirectX11/DirectX11Renderer.hpp>
#include <Graphics/DirectX12/DirectX12Renderer.hpp>
#include <Core/Window.hpp>
#include <memory>

#include <Core/OSDef.hpp>

namespace Sleak {
    namespace RenderEngine {

/// Constructs the concrete Renderer for a requested backend.
class ENGINE_API RendererFactory {
public:
    /// Instantiates and returns the Renderer for the given backend, bound to a window.
    static Renderer* CreateRenderer(RendererType type, Window* window);
    /// Resolves a CLI backend name (e.g. "-vulkan") to a Renderer instance.
    static Renderer* ParseArg(std::string arg, Window* window);
    inline static RendererType GetRendererType() {return Renderertype;}

private:
    static RendererType Renderertype;
};

}
}

#endif // RENDERERFACTORY_HPP
