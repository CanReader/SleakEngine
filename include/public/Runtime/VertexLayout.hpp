#ifndef _VERTEXLAYOUT_HPP_
#define _VERTEXLAYOUT_HPP_

#include <Core/OSDef.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Sleak {

/// Data type of one vertex attribute as seen by the vertex shader.
enum class VertexAttribFormat : uint8_t {
    Float1, Float2, Float3, Float4,
    UInt1, Int1
};

/// One attribute: shader location, component format, byte offset in the vertex.
struct VertexAttribute {
    uint32_t           location;
    VertexAttribFormat format;
    uint32_t           offset;
};

/// A complete custom vertex layout plus the shader stems that consume it.
/// Register once at startup; the handle keys pipelines and mesh creation.
/// Shader stems are consumed by the Vulkan backend; OpenGL instead binds
/// the layout's attributes to its own fixed shader programs.
struct VertexLayoutDesc {
    uint32_t                     stride = 0;
    std::vector<VertexAttribute> attributes;
    std::string                  shaderStem;        // forward/main variant
    // Depth-only shadow variant; empty string skips the shadow pass.
    std::string                  shadowShaderStem;
    // Deferred geometry (GBuffer) variant; empty string is forward only.
    std::string                  gbufferShaderStem;
    // Forward transparent variant; empty string skips the transparent pass.
    std::string                  transparentShaderStem;
};

/// 0 is reserved for the engine's default vertex layout; registered
/// formats start at 1.
using VertexFormatHandle = uint32_t;

/// Global registry of custom vertex layouts. Register returns a stable handle.
class ENGINE_API VertexFormatRegistry {
public:
    static VertexFormatHandle Register(const VertexLayoutDesc& desc);

    /// Returns nullptr for handle 0 or an unknown handle. The returned
    /// pointer stays valid until Shutdown(), which is intended for
    /// application teardown only.
    static const VertexLayoutDesc* Get(VertexFormatHandle handle);

    /// Clears the registry; call only during application teardown.
    static void Shutdown();
};

} // namespace Sleak

#endif
