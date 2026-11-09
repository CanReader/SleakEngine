#ifndef _SHADER_HOT_RELOAD_HPP_
#define _SHADER_HOT_RELOAD_HPP_

#include <Core/OSDef.hpp>
#include <string>

namespace Sleak {
namespace Debug {

/// Recompiles and reloads shaders when their source files are saved.
/// Debug builds only; every call is a no-op in Release.
/// @ingroup debug
class ENGINE_API ShaderHotReload {
   public:
    /// Watches a shader source folder. Shaders in later folders override
    /// earlier ones; includes resolve from the first folder, like glslc -I.
    static void AddSourceDirectory(const std::string& dir);

    /// Rebuilds changed shaders. Called once per frame by Application.
    static void Poll();
};

}  // namespace Debug
}  // namespace Sleak

#endif
