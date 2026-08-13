#ifndef _SHADER_H_
#define _SHADER_H_

#include <Core/OSDef.hpp>
#include <string>

namespace Sleak {
    namespace RenderEngine {
    
/// Backend-agnostic compiled shader program.
class ENGINE_API Shader {
public:
    virtual ~Shader() = default;
    
    /// Compiles a combined shader source file (or backend-specific bundle) at the given path.
    virtual bool compile (const std::string& shaderPath) = 0;
    /// Compiles separate vertex and fragment source files.
    virtual bool compile(const std::string& vert, const std::string& frag) = 0;
    /// Binds this program as the active shader for subsequent draws.
    virtual void bind() = 0;
};

    }
}

#endif