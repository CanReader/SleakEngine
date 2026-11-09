#ifndef _SHADER_INCLUDES_HPP_
#define _SHADER_INCLUDES_HPP_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Sleak {
namespace RenderEngine {

/// Folder under a shader root that holds the `#include` libraries.
inline constexpr std::string_view SHADER_INCLUDE_DIR = "include";
/// Deepest `#include` nesting accepted.
inline constexpr int MAX_SHADER_INCLUDE_DEPTH = 16;

/// Target of one `#include` line.
struct ShaderIncludeDirective {
    std::string name;
    bool quoted = true;
};

/// GLSL with its `#include` lines inlined, for drivers without include support.
struct ExpandedShader {
    std::string source;
    std::vector<std::string> files;
    std::string error;
};

/// Parses an `#include` line with its comments already removed; empty for
/// every other line.
std::optional<ShaderIncludeDirective> ParseShaderInclude(std::string_view line);

/// Finds an include next to its includer (quoted form), then in each root in
/// order. Empty for absolute names and names with a .. part.
std::optional<std::filesystem::path> ResolveShaderInclude(
    const ShaderIncludeDirective& include,
    const std::filesystem::path& includerDir,
    const std::vector<std::filesystem::path>& roots);

/// Inlines every `#include` of a GLSL file, each file once per shader as its
/// guard would. Skips commented-out includes but resolves those under `#if`,
/// keeps `#version` first and tags each file with `#line`.
ExpandedShader ExpandShaderIncludes(
    const std::filesystem::path& file,
    const std::vector<std::filesystem::path>& roots);

/// Replaces the source-string numbers that start driver log lines with the
/// files they stand for.
std::string MapShaderLog(std::string_view log,
                         const std::vector<std::string>& files);

/// True when a shader includes target, directly or through other includes.
bool ShaderDependsOn(const std::filesystem::path& shader,
                     const std::filesystem::path& target,
                     const std::vector<std::filesystem::path>& roots);

}  // namespace RenderEngine
}  // namespace Sleak

#endif
