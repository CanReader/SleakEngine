#ifndef _RENDERABLE_HPP
#define _RENDERABLE_HPP

#include <Core/OSDef.hpp>

namespace Sleak {

    /// Interface for anything the renderer can submit a draw for, ordered by Priority.
    class ENGINE_API Renderable {
        public:
            virtual ~Renderable() = default;
            /// Issues this object's draw commands for the current pass.
            virtual void Render() const = 0;

            int Priority;
    };

}

#endif
