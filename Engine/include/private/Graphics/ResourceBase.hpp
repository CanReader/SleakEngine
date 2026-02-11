#ifndef _RESOURCEBASE_HPP_
#define _RESOURCEBASE_HPP_

#include <Core/OSDef.hpp>
#include <string>

class ENGINE_API ResourceBase {
    public:
        virtual ~ResourceBase() = default;
        virtual bool Initialize(void* Data) = 0;
        virtual void Update() = 0;
        virtual void Cleanup() = 0;
    protected:
        std::string Name;
        bool bIsInitialized;
};

#endif