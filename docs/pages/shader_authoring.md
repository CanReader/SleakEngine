# Shader Authoring {#shader_authoring}

Four backends means four shading languages. A single logical shader such as
`default_shader` exists on disk as up to eight files, and the engine picks
between them by rewriting the file extension at load time. Nothing
cross-compiles, and nothing falls back, so a variant you forget to write is
a pass that silently stops drawing on that backend.

| Backend | Files for stem `foo` | Compiled |
| :--- | :--- | :--- |
| Vulkan | `foo.vert`, `foo.frag`, plus `foo.vert.spv`, `foo.frag.spv` | Ahead of time by `glslc`, committed as `.spv` |
| OpenGL | `foo_gl.vert`, `foo_gl.frag` | At load, by the GL driver |
| DirectX 11 | `foo.hlsl` | At load, by `D3DCompileFromFile` |
| DirectX 12 | `foo_dx12.hlsl` | At load, by `D3DCompileFromFile` |

---

## 1. How a Path Becomes a Shader

Everything starts from one canonical string, the `.hlsl` name, which is what
`Sleak::Material::SetShader` takes:

```cpp
material->SetShader("assets/shaders/flat_shader.hlsl");
```

`ResourceManager::CreateShader` hands that path to whichever backend factory
the active renderer registered, and each backend strips `.hlsl` and appends
its own suffixes.

\dot
digraph shaderpaths {
  bgcolor="transparent"; rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];
  src [label="Material::SetShader\n\"assets/shaders/foo.hlsl\"", fillcolor="#22d3ee22", color="#22d3ee"];
  vk  [label="foo.vert.spv\nfoo.frag.spv"];
  gl  [label="foo_gl.vert\nfoo_gl.frag"];
  d11 [label="foo.hlsl\nVS_Main / PS_Main"];
  d12 [label="foo_dx12.hlsl\nVS_Main / PS_Main"];
  src -> vk  [label="Vulkan"];
  src -> gl  [label="OpenGL"];
  src -> d11 [label="DirectX 11"];
  src -> d12 [label="DirectX 12"];
}
\enddot

Paths are relative and resolve against the process working directory, not
the executable. The engine expects that directory to contain `assets/`, which
is why a game's `main()` normally chdirs to its own executable directory
before doing anything else.

Because the rewrite happens per backend, a stem is only as portable as its
least-covered variant. Adding a shader means writing all four, or knowingly
accepting that the backends you skipped will not render it.

---

## 2. GLSL and SPIR-V

Vulkan consumes `.spv` binaries only. The GLSL sources sit beside them and
are compiled by `glslc` with no flags:

```
glslc assets/shaders/foo.vert -o assets/shaders/foo.vert.spv
```

The output name keeps the full source name and appends `.spv`, so
`foo.vert` becomes `foo.vert.spv`, not `foo.spv`. Getting this wrong
produces a file the loader never looks for.

`.spv` files are committed to the repository. They are build outputs living
in the source tree, which is unusual but deliberate: it keeps a checkout
runnable on Vulkan without the Vulkan SDK installed.

### The engine's CompileShaders target

The engine's `CMakeLists.txt` looks for `glslc`, and when it finds it,
globs `assets/shaders/*.vert` and `assets/shaders/*.frag`, filters out
anything ending in `_gl.vert` or `_gl.frag`, and emits one custom command
per shader that compiles it in place and copies the result into
`bin/assets/shaders`. The commands are collected into an `ALL` target named
`CompileShaders`, and `Engine` depends on it, so it runs before the library
links on every default build.

Three limits are worth knowing before you rely on it:

- **Engine scope only.** The glob is rooted at the engine's own
  `assets/shaders`. A game's shader folder is invisible to it. Games compile
  their own shaders through their own script or target.
- **No `CONFIGURE_DEPENDS`.** The glob is evaluated at configure time, so a
  newly added engine shader needs a CMake re-run before the build sees it.
  Editing an existing shader is tracked correctly and recompiles on the next
  build.
- **Silent when `glslc` is absent.** Without the compiler the target is never
  created at all, and the build prints only
  `glslc not found, SPIR-V shaders will not be auto-compiled` at configure
  time. Every `.spv` then stays at whatever revision was committed, which
  looks exactly like a shader edit that had no effect.

### Game shader folders

A project that ships its own shaders supplies its own compile step, since
the engine target will not touch them. SleakCraft's
`scripts/compile_shaders.sh` is the reference shape: it walks both
`Engine/assets/shaders` and `Game/assets/shaders`, skips any file whose name
matches `*_gl.*`, runs `glslc <src> -o <src>.spv`, and reports failures
without aborting the loop, exiting non-zero if any shader failed or if
nothing was compiled at all. Run it after every edit to a game `.vert` or
`.frag`, since no build step will do it for you.

---

## 3. HLSL

Both DirectX backends compile HLSL at load time through
`D3DCompileFromFile` against the `vs_5_0` and `ps_5_0` profiles. There is no
FXC or DXC build step and no `.cso` artifacts, so an HLSL edit takes effect
as soon as the file reaches the runtime directory.

Entry points are `VS_Main` and `PS_Main`, capitalized exactly:

```hlsl
VS_OUTPUT VS_Main(VS_INPUT input)   { /* ... */ }
float4    PS_Main(VS_OUTPUT input) : SV_Target { /* ... */ }
```

DirectX 11 reads `foo.hlsl` unchanged. DirectX 12 reads `foo_dx12.hlsl`,
which is a separate file rather than a preprocessor branch, mostly because
the two differ in resource binding rather than in shading math.

Diagnostics on the DirectX backends are thin. When a file is missing,
`D3DCompileFromFile` usually returns without an error blob, and the engine
logs only `Failed to compile vertex shader!` or
`Failed to compile pixel shader!` with no path attached. If a DirectX
backend loses a material and the log gives you nothing to work with, check
first that the variant file exists under the exact expected name.

---

## 4. GLSL for OpenGL

OpenGL loads `foo_gl.vert` and `foo_gl.frag` as GLSL text and compiles them
in the driver. Its error reporting is the best of the four:
`Cannot open shader file: {path}` names the missing file,
`Failed to read shader files: {vert} and {frag}` names both, and
`Shader compilation failed: {log}` and `Shader program linking failed: {log}`
forward the driver's own diagnostics.

The `_gl` variants are never passed to `glslc`. Both the engine's CMake
target and SleakCraft's script filter them out by name, since desktop GLSL
is not valid SPIR-V input.

---

## 5. Custom Vertex Formats and Shader Stems

A game that registers its own vertex layout names its shaders through
`Sleak::VertexLayoutDesc`:

```cpp
struct VertexLayoutDesc {
    uint32_t                     stride = 0;
    std::vector<VertexAttribute> attributes;
    std::string                  shaderStem;             // forward opaque
    std::string                  shadowShaderStem;       // empty skips the shadow pass
    std::string                  gbufferShaderStem;      // empty means forward only
    std::string                  transparentShaderStem;  // empty skips the transparent pass
};
```

`Sleak::VertexFormatRegistry::Register(desc)` returns a
`VertexFormatHandle`, and `Sleak::MeshBatch::CreateMesh` keys GPU buffers to
it. See @ref vertex_format for the registration mechanics.

**The stems are Vulkan-only.** Only `VulkanPipelines.cpp` reads them, to
build up to four pipelines per registered format. OpenGL ignores every stem
field and uses the layout purely to program `glVertexAttribPointer` against
its own fixed programs, which is why a game's custom formats need no `_gl`
variants for their gbuffer and shadow stems. Neither DirectX backend
references the registry at all.

How Vulkan turns each stem into a path is not uniform, and the asymmetry
decides which files you have to write:

| Stem | Path built | Stages |
| :--- | :--- | :--- |
| `shaderStem` | `assets/shaders/<stem>` | Both, so `<stem>.vert.spv` and `<stem>.frag.spv` |
| `transparentShaderStem` | `assets/shaders/<stem>` | Both, same as above |
| `shadowShaderStem` | `assets/shaders/<stem>.vert.spv` | Vertex only |
| `gbufferShaderStem` | `assets/shaders/<stem>.vert.spv` | Vertex only; the fragment stage is fixed to the engine's `gbuffer.frag.spv` |

A shadow or gbuffer stem therefore ships as a lone `.vert` plus its `.spv`,
with no fragment shader and no non-Vulkan variants.

### No fallback

An empty stem means "skip that pass for this format", which is a supported
configuration. A stem that is named but fails to load is an error, and the
engine treats the two the same way at draw time: the pass is skipped and
nothing renders through it.

```
VulkanRenderer: Vertex format {} has no main shader stem
VulkanRenderer: Failed to compile '{path}' for vertex format {}
```

Each variant is attempted exactly once per format. A failure sets a sticky
flag so the renderer does not retry compilation every frame, and draws bound
to the missing variant are dropped rather than rasterized through some other
format's pipeline. Nothing substitutes a default shader in.

Two consequences follow. First, the failure is quiet after the first frame:
one error in the log, then geometry that is simply absent, which reads like
a culling bug rather than a shader bug. Second, the main variant gates
everything. A format whose `shaderStem` fails to compile is unbound in every
pass, including shadow and gbuffer variants that compiled without trouble.
The sticky flags do clear when pipelines are destroyed on swapchain
recreation or shutdown, so a resize re-attempts compilation.

---

## 6. Keeping Variants in Sync

Grep for the stem before you touch anything:

```bash
ls assets/shaders/ | grep '^foo'
```

Then edit every file that turns up, plus any HLSL variant a backend still
needs. The engine's own shader set is not uniformly covered, and the gaps
are informative: `ssr` and `taa` are Vulkan-only, `gbuffer` and
`lighting_pass` have no HLSL, and the three `ibl_*` stems and
`skinned_shader` have no `_dx12.hlsl`. Those features are unavailable on the
backends whose variant is absent, which matches what
@ref backend_support reports.

A working checklist for a new shader stem:

1. Write `foo.vert` and `foo.frag` (Vulkan GLSL).
2. Write `foo_gl.vert` and `foo_gl.frag` (desktop GLSL).
3. Write `foo.hlsl` and `foo_dx12.hlsl` with `VS_Main` and `PS_Main`.
4. Build, or run the project's shader script, to produce `foo.vert.spv` and
   `foo.frag.spv`.
5. Commit the `.spv` files alongside the sources.
6. Launch once per backend with `-r vulkan`, `-r opengl`, `-r d3d11`, and
   `-r d3d12`, and read the log rather than trusting the picture.

Step six catches the failure mode the others cannot. A missing variant does
not break the build, does not crash, and on three of the four backends does
not even name the file it wanted.

---

## 7. Where to Look in the Source

| Question | File |
| :--- | :--- |
| Extension rewrite per backend | The single-argument `compile()` in each `src/Graphics/<backend>/` shader class |
| SPIR-V build target and its scope | `CMakeLists.txt`, the `find_program(GLSLC ...)` block |
| Stem to pipeline mapping and error text | `src/Graphics/Vulkan/VulkanPipelines.cpp`, `CreateCustomFormatPipelines` |
| Sticky failure flags and draw suppression | `include/private/Graphics/Vulkan/VulkanRenderer.hpp`, `CustomFormatPipelines` |
| OpenGL attribute binding from a layout | `src/Graphics/OpenGL/OpenGLRenderer.cpp`, `BindVertexBuffer` |
| The layout description itself | `include/public/Runtime/VertexLayout.hpp` |
