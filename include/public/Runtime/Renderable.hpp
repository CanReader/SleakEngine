#ifndef _RENDERABLE_HPP
#define _RENDERABLE_HPP

#include <Core/OSDef.hpp>

namespace Sleak {

    class ENGINE_API Renderable {
        public:
            virtual ~Renderable() = default;
            virtual void Render() const = 0;

            int Priority;
    };

}

#endif
