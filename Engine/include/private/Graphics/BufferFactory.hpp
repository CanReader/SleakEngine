#ifndef _BUFFERFACTORY_HPP
#define _BUFFERFACTORY_HPP


#include "BufferBase.hpp"
#include <Core/OSDef.hpp>
#include <Graphics/RendererFactory.hpp>
#include <memory>

namespace Sleak {
    namespace RenderEngine {

class ENGINE_API BufferFactory {
public:
    static BufferBase* CreateBuffer(RendererType type);
};

}
}

#endif // _BUFFERFACTORY_HPP
