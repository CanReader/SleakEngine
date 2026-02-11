#ifndef _DRAWABLE_HPP_
#define _DRAWABLE_HPP_

#include <Core/OSDef.h>
#include <string>

class IDrawable {
    public:
        virtual bool Initialize() = 0;
        virtual void Update() = 0;
        virtual void Cleanup() = 0;
    protected:
        std::string Name;
        bool bIsInitialized;
};

#endif