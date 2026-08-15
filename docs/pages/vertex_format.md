# Vertex Format Registration {#vertex_format}

SleakEngine does not mandate a fixed vertex format. It keeps one built-in
default vertex layout internally (`VertexFormatHandle` `0`), and downstream
applications can register their own custom layouts at runtime instead of
using it.

| Type | Role |
| :--- | :--- |
| `Sleak::VertexLayoutDesc` | Stride, attribute list, and up to four shader stems describing one layout. |
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
`vertexFormat` is `0`. It never reads any of the four shader stems; shader
program selection for OpenGL draws is entirely independent of the
registered format and uses fixed, hardcoded programs per pass. This is why
custom vertex formats that only add the four stems (main/shadow/gbuffer/
transparent) do not need an OpenGL-specific shader variant.

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

Handles are process-lifetime values held by the registry. `Shutdown` clears
it, so any `MeshHandle` still carrying a handle after that point refers to
nothing.

---

## 5. Where to Look in the Source

| Question | File |
| :--- | :--- |
| The descriptor, handle, and registry API | `include/public/Runtime/VertexLayout.hpp` |
| Mesh creation and drawing | `include/public/Runtime/MeshBatch.hpp`, `src/Graphics/Common/MeshBatch.cpp` |
| How the four stems become Vulkan pipelines | `src/Graphics/Vulkan/VulkanPipelines.cpp` |
| How OpenGL binds attributes generically | `src/Graphics/OpenGL/OpenGLRenderer.cpp` |
| The engine's own default vertex type | `include/public/Runtime/MeshData.hpp` |
| A real registration in a shipped game | SleakCraft, `Game/include/World/VoxelVertex.hpp` |
