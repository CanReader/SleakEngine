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
struct VertexLayoutDesc {
    uint32_t                     stride = 0;
    std::vector<VertexAttribute> attributes;
    std::string                  shaderStem;         // forward/main variant
    std::string                  shadowShaderStem;   // depth-only shadow variant ("" = no shadows)
    std::string                  gbufferShaderStem;  // deferred geometry variant ("" = forward only)
};

/// 0 is reserved for the engine's default Vertex layout; registered formats start at 1.
using VertexFormatHandle = uint32_t;

/// Global registry of custom vertex layouts. Register returns a stable handle.
class ENGINE_API VertexFormatRegistry {
public:
    static VertexFormatHandle Register(const VertexLayoutDesc& desc);
    static const VertexLayoutDesc* Get(VertexFormatHandle handle);   // nullptr for 0/unknown
    static void Shutdown();                                          // clears the registry
};

} // namespace Sleak

#endif
