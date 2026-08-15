#ifndef _VERTEXLAYOUT_HPP_
#define _VERTEXLAYOUT_HPP_

#include <Core/OSDef.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Sleak {

/// Data type of one vertex attribute as seen by the vertex shader.
/// @ingroup vertexformats
enum class VertexAttribFormat : uint8_t {
    Float1, Float2, Float3, Float4,
    UInt1, Int1
};

/// One attribute: shader location, component format, byte offset in the vertex.
/// @ingroup vertexformats
struct VertexAttribute {
    uint32_t           location;
    VertexAttribFormat format;
    uint32_t           offset;
};

/// A complete custom vertex layout plus the shader stems that consume it.
/// Register once at startup; the handle keys pipelines and mesh creation.
/// Shader stems are consumed by the Vulkan backend; OpenGL instead binds
/// the layout's attributes to its own fixed shader programs.
///
/// Fill one of these when your geometry does not fit the engine's built-in
/// vertex type: packed voxel vertices, extra UV sets, per-vertex instance
/// data, anything you want to define yourself. `stride` is the size of one
/// vertex in bytes and each VertexAttribute gives a shader location, a
/// component format, and a byte offset inside that vertex.
///
/// The four shader stems name the pipeline variants the format
/// participates in. An empty stem skips that pass entirely, so a format
/// with no `shadowShaderStem` casts no shadows and a format with no
/// `gbufferShaderStem` is forward only. There is no fallback shader: if a
/// named stem fails to load, the backend logs an error and skips that pass
/// for this format.
///
/// @code{.cpp}
/// struct VoxelVertex {
///     float    position[3];
///     float    normal[3];
///     float    uv[2];
///     uint32_t packedLight;
/// };
///
/// Sleak::VertexLayoutDesc desc;
/// desc.stride = sizeof(VoxelVertex);
/// desc.attributes = {
///     {0, Sleak::VertexAttribFormat::Float3,
///      offsetof(VoxelVertex, position)},
///     {1, Sleak::VertexAttribFormat::Float3,
///      offsetof(VoxelVertex, normal)},
///     {2, Sleak::VertexAttribFormat::Float2, offsetof(VoxelVertex, uv)},
///     {3, Sleak::VertexAttribFormat::UInt1,
///      offsetof(VoxelVertex, packedLight)},
/// };
/// desc.shaderStem            = "flat_shader";
/// desc.shadowShaderStem      = "shadow_depth_voxel";
/// desc.gbufferShaderStem     = "gbuffer_voxel";
/// desc.transparentShaderStem = "water_shader";
///
/// Sleak::VertexFormatHandle handle =
///     Sleak::VertexFormatRegistry::Register(desc);
/// @endcode
///
/// @see VertexFormatRegistry, VertexAttribute, VertexAttribFormat, MeshBatch
/// @ingroup vertexformats
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
///
/// This is how a game teaches the renderer about its own geometry. Call
/// Register() once per layout during startup, keep the returned
/// VertexFormatHandle, and pass it to MeshBatch::CreateMesh(). The handle
/// travels on the resulting MeshHandle, and the backend uses it to pick
/// the pipeline for every pass the format participates in.
///
/// Handles are stable for the lifetime of the process and start at 1;
/// handle 0 is reserved for the engine's built-in vertex layout. Register()
/// does not validate the descriptor. A zero stride or an empty attribute
/// list is caught later, at pipeline creation.
///
/// Register once and reuse the handle. Registering the same descriptor
/// repeatedly creates a new handle and a new set of pipelines each time.
/// Shutdown() is for application teardown only, and pointers returned by
/// Get() stay valid until it runs.
///
/// @code{.cpp}
/// // Startup: register the format your game uses
/// class VoxelVertexFormat {
/// public:
///     static Sleak::VertexFormatHandle Handle() {
///         static Sleak::VertexFormatHandle handle = Register();
///         return handle;
///     }
///
/// private:
///     static Sleak::VertexFormatHandle Register() {
///         Sleak::VertexLayoutDesc desc;
///         desc.stride = sizeof(VoxelVertex);
///         desc.attributes = MakeVoxelAttributes();
///         desc.shaderStem = "flat_shader";
///         return Sleak::VertexFormatRegistry::Register(desc);
///     }
/// };
///
/// // Mesh creation: key the buffers to that handle
/// Sleak::MeshHandle mesh = Sleak::MeshBatch::CreateMesh(
///     VoxelVertexFormat::Handle(),
///     verts.data(), verts.size() * sizeof(VoxelVertex),
///     indices.data(), indices.size());
///
/// // Teardown
/// Sleak::VertexFormatRegistry::Shutdown();
/// @endcode
///
/// @see VertexLayoutDesc, VertexFormatHandle, MeshBatch, MeshHandle
/// @ingroup vertexformats
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
