#ifndef _SHADER_H_
#define _SHADER_H_

#include <Core/OSDef.hpp>
#include <string>

namespace Sleak {
    namespace RenderEngine {
    
class ENGINE_API Shader {
public:
    virtual ~Shader() = default;
    
    virtual bool compile (const std::string& shaderPath) = 0;
    virtual bool compile(const std::string& vert, const std::string& frag) = 0;
    virtual void bind() = 0;
};

    }
}

#endif