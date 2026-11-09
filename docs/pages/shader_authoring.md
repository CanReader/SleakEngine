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
are compiled by `glslc` with the include library on the search path:

```
glslc -I assets/shaders/include assets/shaders/foo.vert \
    -o assets/shaders/foo.vert.spv
```

The output name keeps the full source name and appends `.spv`, so
`foo.vert` becomes `foo.vert.spv`, not `foo.spv`. Getting this wrong
produces a file the loader never looks for.

`.spv` files are committed to the repository, but only as a fallback that
keeps a checkout runnable on Vulkan without the Vulkan SDK installed. A
build with `glslc` never reads or writes them. CI fails when a change to a
Vulkan `.vert` or `.frag`, or to an include it uses, lands without its
`.spv`, unless the edit compiles to the same code (a comment), so recompile
and commit the `.spv` with every GLSL edit.

### The engine's CompileShaders target

The engine's `CMakeLists.txt` globs `assets/shaders/*.vert` and
`assets/shaders/*.frag` with `CONFIGURE_DEPENDS`, filters out anything ending
in `_gl.vert` or `_gl.frag`, and emits one custom command per shader that
compiles it into `<build>/Engine/shaders` with `-I assets/shaders/include`.
Every shader depends on every file in the include library, so an include
edit rebuilds them all. The commands are collected into an
`ALL` target named `CompileShaders`, and `Engine` depends on it.

Staging depends on how the engine is built:

- **Standalone.** The compiled `.spv` are copied into `bin/assets/shaders`.
- **Through `add_subdirectory`.** Once the consuming project is configured,
  every executable that links `Engine` directly gets a
  `<target>_EngineSpirv` target that copies the compiled `.spv` into
  `assets/shaders` next to the executable. It runs after the executable and
  its own asset copy, so a stale committed `.spv` copied by the game never
  overwrites the fresh one.

Two limits are worth knowing before you rely on it:

- **Engine scope only.** The glob is rooted at the engine's own
  `assets/shaders`. A game's shader folder is invisible to it. Games compile
  their own shaders through their own script or target; the global
  properties `ENGINE_SHADER_INCLUDE_DIR` and `ENGINE_SHADER_INCLUDES` give
  that step the include folder and the files to depend on.
- **Fallback when `glslc` is absent.** Configure warns and stages the
  committed `.spv` instead, so GLSL edits have no effect until `glslc` is
  installed. If a shader has no committed `.spv` either, configure fails.

### Game shader folders

A project that ships its own shaders supplies its own compile step, since
the engine target will not touch them. SleakCraft is the reference shape:
its game CMake target compiles `Game/assets/shaders/*.vert` and `*.frag`
into the build tree with both include folders and stages the results over
the committed copies, and `scripts/compile_shaders.sh` refreshes the
committed `.spv` of both folders (`glslc -I ... <src> -o <src>.spv`),
exiting non-zero if any shader failed or if nothing was compiled at all.
Run the script before committing a GLSL or include edit.

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
in the driver. Before that, the loader inlines every `#include` from
`assets/shaders/include` (the runtime copy of the include folders), keeps
`#version` first, drops the `GL_GOOGLE_include_directive` line and marks
each file with `#line`, so driver errors name the include file and line.
Its error reporting is the best of the four:
`Cannot read shader {path}: {reason}` names the missing file or include,
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

## 7. Shared Includes and FrameData

Shared GLSL lives in `assets/shaders/include/<namespace>/`: `sleak/` for
the engine, a game namespace (SleakCraft uses `craft/`) for game code.
Vulkan resolves includes at compile time and OpenGL at load time, so the
same file serves both. The engine library holds:

| File | Contents |
| :--- | :--- |
| `sleak/binding.glsl` | Slot numbers, `SLEAK_BINDING(set, vkBinding, glBinding)` keyed on glslc's `VULKAN` macro, depth-range and UV/clip flip helpers |
| `sleak/frame.glsl` | The `FrameData` block (`uFrame`), `SLEAK_FRAME_USER_BLOCK`, the Debug guards |
| `sleak/shadow_light.glsl` | The full `ShadowLightUBO` block |

Rules: a file that uses `#include` declares
`#extension GL_GOOGLE_include_directive : require` right after `#version`;
include files never contain `#version` or `#extension` and use
`#ifndef SLEAK_*_GLSL` guards. Include names are relative to an include
folder, never absolute and never through `..`. A name is looked up next to
the including file, then in the engine folder, then in the game's, the order
`glslc -I` uses. A game must not ship its own `include/sleak/` (SleakCraft's
CMake refuses it), so nothing shadows the engine library.

The OpenGL resolver inlines each file once per shader, as its guard would,
reports an include cycle between unguarded files, and skips `#include` lines
inside `//` and `/* */` comments. It does not evaluate `#if`: an `#include`
under a false `#if` is still resolved, so the file must exist.

Per-frame data sits in descriptor set 2 on Vulkan and in uniform buffers
11-15 on OpenGL:

| Block | Vulkan | OpenGL | Contents |
| :--- | :--- | :--- | :--- |
| `ShadowLightUBO` | set 2, binding 0 | 14 | Primary light, shadow and fog |
| `FrameData` | set 2, binding 1 | 11 | Clock, frame index, screen, jitter, camera and matrices, light, flags |
| Game block | set 2, binding 2 | 12 | Up to 1 KiB set with `Application::SetFrameUserData` |
| Cascades | set 2, binding 3 | 13 | Reserved, zero |
| Atmosphere | set 2, binding 4 | 15 | Reserved, zero |

`FrameData` is std140 and append-only. To add a field, append it to the C++
struct in `ConstantBuffer.hpp` with its `static_assert`, append the member
to `sleak/frame.glsl`, bump `FRAME_DATA_VERSION` and
`SLEAK_FRAME_DATA_VERSION` together, and run `scripts/check_ubo_layout.py`,
which also compares the version and every `SLEAK_FRAME_FLAG_*` bit with its
C++ twin. `uFrame.Time` is the engine clock (`Application::GetEngineTime`,
frame / 60 under `-capture`). `ShadowLightUBO.CameraPos.w` is a legacy copy
of it, so it now counts from `Application::Run()` (frame 0 under `-capture`)
instead of from the first `LightManager` update. Activating a scene restarts
the previous-frame fields (`PrevViewProjRel`, `PrevCameraPos`, `Jitter.zw`)
from the new camera and clears the game block. Debug builds set a guard
flag: a fragment shader built against another `FrameData` version draws
magenta, a vertex shader drops its geometry.

---

## 8. Where to Look in the Source

| Question | File |
| :--- | :--- |
| Extension rewrite per backend | The single-argument `compile()` in each `src/Graphics/<backend>/` shader class |
| SPIR-V build target and its scope | `CMakeLists.txt`, the `find_program(GLSLC ...)` block |
| Stem to pipeline mapping and error text | `src/Graphics/Vulkan/VulkanPipelines.cpp`, `CreateCustomFormatPipelines` |
| Sticky failure flags and draw suppression | `include/private/Graphics/Vulkan/VulkanRenderer.hpp`, `CustomFormatPipelines` |
| OpenGL attribute binding from a layout | `src/Graphics/OpenGL/OpenGLRenderer.cpp`, `BindVertexBuffer` |
| The layout description itself | `include/public/Runtime/VertexLayout.hpp` |
| Include library | `assets/shaders/include/sleak/` |
| OpenGL include resolver and log mapping | `src/Graphics/Common/ShaderIncludes.cpp` |
| `FrameData` layout and fill | `include/private/Graphics/Common/ConstantBuffer.hpp`, `src/Graphics/Common/FrameData.cpp` |
