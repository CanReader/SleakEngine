#ifndef _RESOURCEBASE_HPP_
#define _RESOURCEBASE_HPP_

#include <Core/OSDef.hpp>
#include <string>

namespace Sleak {

/// Common lifecycle contract for GPU-backed resources: init, per-frame update, teardown.
class ENGINE_API ResourceBase {
    public:
        virtual ~ResourceBase() = default;
        /// Allocates backend resources from backend-specific creation parameters.
        virtual bool Initialize(void* Data) = 0;
        virtual void Update() = 0;
        virtual void Cleanup() = 0;
    protected:
        std::string Name;
        bool bIsInitialized;
};

}

#endif