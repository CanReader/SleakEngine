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

/// Face culling of one custom-format pipeline variant.
/// @ingroup vertexformats
enum class VertexCullMode : uint8_t {
    Default,  ///< The backend's existing choice for the pass
    Back,     ///< Cull faces wound clockwise on screen
    Front,    ///< Cull faces wound counter-clockwise on screen
    None      ///< Draw both sides
};

/// A complete custom vertex layout plus the shader stems that consume it.
/// Register once at startup; the handle keys pipelines and mesh creation.
/// Vulkan loads the stems as SPIR-V; OpenGL loads them with a `_gl` suffix
/// where those files exist (see below).
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
/// Set `transparentReadsScene` for transparent geometry that refracts or
/// reflects what lies behind it, such as water. The first transparent draw
/// of the format in a frame snapshots the scene drawn so far, and its
/// shaders can then read it:
/// - Vulkan, descriptor set 3: binding 2 scene color (linear HDR),
///   binding 3 scene depth, binding 4 sky cubemap, binding 5 the
///   SceneReadUBO block below.
/// - OpenGL: texture unit 4 scene color (tonemapped, as shown), unit 5
///   scene depth, unit 6 sky cubemap, uniform block binding 4 SceneReadUBO.
///
/// @code{.glsl}
/// uniform SceneReadUBO {
///     mat4 ViewProj;     // unjittered, row-major engine matrix
///     mat4 InvViewProj;
///     vec4 ScreenSize;   // width, height, 1/width, 1/height
///     vec4 Camera;       // TAA jitter in NDC (xy), near, far
///     vec4 Flags;        // x: scene textures valid, y: sky valid
/// };
/// @endcode
///
/// Add `Camera.xy * gl_Position.w` to the clip position after the Vulkan Y
/// flip so the draw lines up with the jittered opaque geometry.
///
/// On Vulkan, transparent draws set the HDR scene alpha to 0. TAA reads that
/// as a reactive mask and trusts history less there, since animated or
/// refracted content is not described by the depth buffer.
///
/// `gbufferFragmentStem` replaces the GBuffer variant's engine fragment
/// shader and must write all four targets; RT3 holds object motion, a game
/// channel and a shading model id (`sleak/gbuffer.glsl`). Without it RT3
/// keeps shading model 0, which game lighting stems shade like engine
/// materials.
///
/// `shadowFragmentStem` makes the shadow variant alpha-tested: it reads the
/// caster's diffuse (Vulkan set 0 binding 0, OpenGL unit 0) and discards.
/// Shadows replay the previous frame, so that material must outlive its last
/// draw by one frame.
///
/// `cullMode` covers the main and GBuffer variants, `shadowCullMode` the
/// shadow variant; transparent variants draw both sides. `Default` keeps the
/// backend's choice. Vulkan shadow shaders do not flip Y, so there `Back`
/// culls the faces turned toward the light; OpenGL culls those turned away.
///
/// OpenGL builds `<stem>_gl.vert` and `.frag` for the shadow, GBuffer and
/// transparent passes, with the engine `shadow_depth_gl` and `gbuffer_gl`
/// fragments for empty fragment stems. A pass without its `_gl.vert` keeps
/// the engine program; the deferred-off main pass uses the material's.
///
/// @see VertexFormatRegistry, VertexAttribute, VertexAttribFormat,
/// VertexCullMode, MeshBatch
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
    // Transparent draws sample the opaque scene; see above.
    bool transparentReadsScene = false;
    /// Fragment stem of the GBuffer variant; empty runs the engine's.
    std::string gbufferFragmentStem;
    /// Fragment stem of the shadow variant; empty keeps it depth only.
    std::string shadowFragmentStem;
    /// Culling of the main and GBuffer variants.
    VertexCullMode cullMode = VertexCullMode::Default;
    /// Culling of the shadow variant.
    VertexCullMode shadowCullMode = VertexCullMode::Default;
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
