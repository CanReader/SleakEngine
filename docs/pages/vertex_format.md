# Vertex Format Registration {#vertex_format}

SleakEngine does not mandate a fixed vertex format. It keeps one built-in
default vertex layout internally (`VertexFormatHandle` `0`), and downstream
applications can register their own custom layouts at runtime instead of
using it.

| Type | Role |
| :--- | :--- |
| `Sleak::VertexLayoutDesc` | Stride, attribute list, up to four shader stems, two fragment stems and two cull modes describing one layout. |
| `Sleak::VertexCullMode` | Face culling of one pipeline variant; `Default` keeps the backend's choice. |
| `Sleak::VertexAttribute` | One attribute: shader location, component format, byte offset. |
| `Sleak::VertexAttribFormat` | The component types an attribute can have. |
| `Sleak::VertexFormatRegistry` | All-static registry; `Register` returns a handle, `Get` reads a desc back. |
| `Sleak::VertexFormatHandle` | The `uint32_t` key everything downstream is addressed by; `0` is the engine default. |
| `Sleak::MeshBatch` | Builds GPU buffers for a handle and draws the result. |
| `Sleak::MeshHandle` | Vertex buffer, index buffer, index count, and the format tag that picks the pipeline. |

\dot
digraph vformat {
  bgcolor="transparent"; rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];

  desc  [label="VertexLayoutDesc\nstride, attributes,\n4 shader stems"];
  reg   [label="VertexFormatRegistry::Register", fillcolor="#22d3ee22", color="#22d3ee"];
  hnd   [label="VertexFormatHandle", fillcolor="#22d3ee22", color="#22d3ee"];
  create[label="MeshBatch::CreateMesh(handle,\nvertexData, bytes, indices, count)"];
  mesh  [label="MeshHandle\nvertexBuffer, indexBuffer,\nindexCount, vertexFormat"];
  draw  [label="MeshBatch::Draw(mesh, castsShadow)"];
  pipes [label="backend pipelines\none per non-empty stem:\nmain, shadow, gbuffer, transparent"];

  desc -> reg -> hnd;
  hnd -> create [label="keys the buffers"];
  create -> mesh -> draw;
  hnd -> pipes [label="built once at\npipeline creation"];
  draw -> pipes [label="bound off\nmesh.vertexFormat"];
}
\enddot

Register once at startup, then every mesh and every pipeline is addressed by
the handle you got back.

---

## 1. `Sleak::VertexLayoutDesc`

Defined in `include/public/Runtime/VertexLayout.hpp`:

```cpp
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

/// Face culling of one custom-format pipeline variant.
enum class VertexCullMode : uint8_t { Default, Back, Front, None };

/// A complete custom vertex layout plus the shader stems that consume it.
/// Register once at startup; the handle keys pipelines and mesh creation.
/// Vulkan loads the stems as SPIR-V; OpenGL loads them with a `_gl` suffix
/// where those files exist.
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
    // Transparent draws sample the opaque scene.
    bool transparentReadsScene = false;
    // Fragment stem of the GBuffer variant; empty runs the engine's.
    std::string gbufferFragmentStem;
    // Fragment stem of the shadow variant; empty keeps it depth only.
    std::string shadowFragmentStem;
    // Culling of the main and GBuffer variants, and of the shadow variant.
    VertexCullMode cullMode = VertexCullMode::Default;
    VertexCullMode shadowCullMode = VertexCullMode::Default;
};

/// 0 is reserved for the engine's default vertex layout; registered
/// formats start at 1.
using VertexFormatHandle = uint32_t;

/// Global registry of custom vertex layouts. Register returns a stable handle.
class VertexFormatRegistry {
public:
    static VertexFormatHandle Register(const VertexLayoutDesc& desc);
    static const VertexLayoutDesc* Get(VertexFormatHandle handle);
    static void Shutdown();
};
```

`Register` appends `desc` to an internal list and returns its index as the
handle; it performs no validation of its own (no offset or stride checks
happen at registration time). The only defensive check
(`stride == 0 || attributes.empty()`) happens later, at Vulkan pipeline
creation, in `VulkanRenderer::CreateCustomFormatPipelines`.

---

## 2. Worked Example

A downstream game defining its own vertex struct registers a matching
`VertexLayoutDesc` once at startup:

```cpp
struct MyVertex {
    float position[3];
    float normal[3];
    float uv[2];
};

VertexLayoutDesc desc;
desc.stride = sizeof(MyVertex);
desc.attributes = {
    { 0, VertexAttribFormat::Float3, offsetof(MyVertex, position) },
    { 1, VertexAttribFormat::Float3, offsetof(MyVertex, normal) },
    { 2, VertexAttribFormat::Float2, offsetof(MyVertex, uv) },
};
desc.shaderStem            = "my_shader";
desc.shadowShaderStem      = "shadow_depth_my_shader";
desc.gbufferShaderStem     = "gbuffer_my_shader";
desc.transparentShaderStem = "";  // opaque only, skip the transparent pass

VertexFormatHandle format = VertexFormatRegistry::Register(desc);
```

and creates GPU buffers for a mesh built from that struct through the
raw-bytes overload of `MeshBatch::CreateMesh`
(`include/public/Runtime/MeshBatch.hpp`):

```cpp
static MeshHandle CreateMesh(VertexFormatHandle format,
                              const void* vertexData, size_t vertexBytes,
                              const uint32_t* indices, size_t indexCount);
```

```cpp
std::vector<MyVertex> vertices = /* ... */;
std::vector<uint32_t> indices  = /* ... */;

MeshHandle mesh = MeshBatch::CreateMesh(
    format, vertices.data(), vertices.size() * sizeof(MyVertex),
    indices.data(), indices.size());
```

The returned `MeshHandle` carries `vertexBuffer`, `indexBuffer`,
`indexCount`, and a `vertexFormat` field (`0` means the engine default) so
a renderer knows which pipeline the mesh needs. Drawing goes through
`MeshBatch::BeginBatch(material)` / `MeshBatch::Draw(mesh, castsShadow)` /
`MeshBatch::EndBatch()`, or through `RenderCommandQueue::SubmitDrawIndexed`
directly for components that manage their own constant buffers (as
`MeshComponent` does).

There is also a `VertexGroup&`/`IndexGroup&` overload of `CreateMesh` for
building meshes from the engine's own CPU mesh data types
(`include/public/Runtime/MeshData.hpp`), used for the engine-default vertex
format and primitives loaded through `ModelLoader`.

---

## 3. Pipeline Binding per Backend

**Vulkan** (`VulkanRenderer::CreateCustomFormatPipelines`,
`src/Graphics/Vulkan/VulkanPipelines.cpp`) builds up to four pipeline
variants from the four shader stems, each gated independently. An empty
main `shaderStem` logs an error and marks that variant failed; a shader
compile failure or a failed `vkCreateGraphicsPipelines` call logs an error
and marks that specific variant failed. No fallback shader is ever
substituted; a failed or absent variant simply causes that pass to be
skipped for that vertex format on every frame.

**OpenGL** (`OpenGLRenderer::BindVertexBuffer`,
`src/Graphics/OpenGL/OpenGLRenderer.cpp`) looks up the registered
`VertexLayoutDesc` and emits `glVertexAttribPointer` /
`glVertexAttribIPointer` calls generically from its `attributes` and
`stride`, falling back to the engine's hardcoded default vertex layout when
`vertexFormat` is `0`. `OpenGLRenderer::SelectFormatProgram` then picks the
program for the draw. A stem is used only when its `<stem>_gl.vert` exists;
otherwise the pass keeps the engine program it always used, logged once at
info. A `_gl` file that exists but fails to build logs an error and the
pass skips the format, as on Vulkan.

| Pass | Vulkan | OpenGL |
| :--- | :--- | :--- |
| Shadow | `<shadowShaderStem>.vert.spv`, plus `<shadowFragmentStem>.frag.spv` when set | `<shadowShaderStem>_gl.vert` with `<shadowFragmentStem>_gl.frag` or `shadow_depth_gl.frag`; without the file, the position-only `shadow_position_gl` program |
| GBuffer | `<gbufferShaderStem>.vert.spv` with `<gbufferFragmentStem>.frag.spv` or the engine `gbuffer.frag.spv` | `<gbufferShaderStem>_gl.vert` with `<gbufferFragmentStem>_gl.frag` or `gbuffer_gl.frag`; without the file, the engine GBuffer program |
| Transparent | `<transparentShaderStem>.vert.spv` and `.frag.spv`, both sides drawn | `<transparentShaderStem>_gl.vert` and `_gl.frag`; without the file, the material's program |
| Main, deferred off | `<shaderStem>.vert.spv` and `.frag.spv` | the material's program |

---

## 4. Practical Notes

`Register` neither validates nor deduplicates. Registering the same
descriptor twice yields two distinct handles and two full sets of backend
pipelines, so keep the call in one place and hand the resulting handle
around rather than rebuilding the descriptor at each use site.

An empty shader stem is a deliberate opt-out, not an error. Leave
`transparentShaderStem` empty for an opaque-only format and that pass is
skipped for the format, with no cost and no log noise. An empty main
`shaderStem` is different: it fails pipeline creation and every pass for
that format goes dark.

A `gbufferFragmentStem` writes all four GBuffer targets. The fourth, RT3, is
object motion, a game channel and a shading model id, laid out in
`assets/shaders/include/sleak/gbuffer.glsl`; leaving it unwritten writes
undefined values. Engine GBuffer pipelines mask RT3, so engine materials
read shading model 0, which a game lighting stem shades with the engine
lighting (see @ref lighting). Masking one target alone needs the Vulkan
`independentBlend` device feature. On a device without it the engine GBuffer
fragment shader writes RT3 as zero instead: the same shading model 0, for one
more RGBA16F write (8 bytes) per engine-material fragment. The engine logs
this once at startup.

A `shadowFragmentStem` makes the shadow variant alpha-tested. It samples the
caster material's diffuse at Vulkan set 0 binding 0 or OpenGL texture unit
0 and discards. Shadows replay the previous frame's draws, so that material
has to stay alive one frame after its last draw.

`cullMode` and `shadowCullMode` keep each backend's existing culling while
`Default`. Vulkan shadow shaders do not flip Y, so the light's view is
mirrored there and `Back` culls the faces turned toward the light; OpenGL
culls the faces turned away.

Handles are process-lifetime values held by the registry. `Shutdown` clears
it, so any `MeshHandle` still carrying a handle after that point refers to
nothing.

---

## 5. Where to Look in the Source

| Question | File |
| :--- | :--- |
| The descriptor, handle, and registry API | `include/public/Runtime/VertexLayout.hpp` |
| Mesh creation and drawing | `include/public/Runtime/MeshBatch.hpp`, `src/Graphics/Common/MeshBatch.cpp` |
| How the stems become Vulkan pipelines | `src/Graphics/Vulkan/VulkanPipelines.cpp` |
| The alpha-tested shadow material binding | `src/Graphics/Vulkan/VulkanShadow.cpp`, `RenderCommandQueue::ExecuteShadowPass` |
| How OpenGL binds attributes and picks programs | `src/Graphics/OpenGL/OpenGLRenderer.cpp`, `SelectFormatProgram` |
| The engine's own default vertex type | `include/public/Runtime/MeshData.hpp` |
| A real registration in a shipped game | SleakCraft, `Game/include/World/VoxelVertex.hpp` |
