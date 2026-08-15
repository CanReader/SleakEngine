# Rendering Pipeline {#rendering_pipeline}

SleakEngine has no unified cross-backend hardware abstraction. Rendering is
built around an abstract base class, `Sleak::RenderEngine::Renderer`
(`include/private/Graphics/Common/Renderer.hpp`, a private header), with
four independent backend implementations: `VulkanRenderer`, `OpenGLRenderer`,
`DirectX11Renderer`, `DirectX12Renderer`. Each backend class implements the
`RenderContext` command-recording interface directly on itself (multiple
inheritance, e.g. `class VulkanRenderer : public Renderer, public
RenderContext`), so a backend renderer is its own render context. The four
backends share a lifecycle contract but diverge in real, load-bearing ways
in what they actually implement.

| Type | Role |
| :--- | :--- |
| `Sleak::Application` | Drives the frame: window pump, scene ticks, queue flush, frame boundary. |
| `RenderEngine::Renderer` | Abstract backend lifecycle: `Initialize`, `BeginRender`, `EndRender`, `Resize`, `Cleanup`. |
| `RenderEngine::RenderContext` | Abstract command-recording surface; each backend implements it on itself. |
| `RenderEngine::RendererFactory` | Picks and constructs the backend from a `RendererType` or a CLI string. |
| `RenderEngine::RenderCommandQueue` | Singleton frame queue of draws and state changes, replayed into the context. |
| `RenderEngine::Shader` / `BufferBase` | Backend-agnostic compiled shader and GPU buffer handles. |
| `Sleak::Texture` / `CubemapTexture` | Sampled image resources, the cubemap implemented per backend. |

\dot
digraph frame {
  bgcolor="transparent"; rankdir=TB;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];

  win   [label="Window::Update()\nSDL pump, resize applied"];
  begin [label="Renderer::BeginRender()\nshadow replay, open GBuffer pass", fillcolor="#22d3ee22", color="#22d3ee"];
  scene [label="Scene FixedUpdate / Update / LateUpdate\nGameBase::Loop"];
  queue [label="RenderCommandQueue\nSubmitDraw, SubmitDrawIndexed, ..."];
  flush [label="FlushPendingTransfers()\nExecuteCommands(context)"];
  gbuf  [label="GBuffer pass\nopaque draws inline\nforward and skinned deferred"];
  light [label="ExecuteDeferredLightingPass()\nSSAO, then fullscreen lighting"];
  fwd   [label="BeginForwardTransparentPass()\ntransparent, skinned, skybox, debug lines"];
  post  [label="post chain\nTAA -> SSR -> bloom -> composite"];
  end   [label="Renderer::EndRender()\nsubmit and present", fillcolor="#22d3ee22", color="#22d3ee"];

  win -> begin -> scene;
  scene -> queue [label="submit"];
  queue -> flush [label="replay"];
  flush -> gbuf -> light -> fwd;
  fwd -> post [label="inside EndRender"];
  post -> end;
  end -> win [label="next frame", style=dashed];
}
\enddot

The frame boundary is `BeginRender` and `EndRender` with the queue replayed
between them. There is no `RenderFrame` method and no separate `Present`
call.

---

## 1. Renderer Base and Factory

`Renderer`'s real interface:

```cpp
virtual bool Initialize() = 0;                          // device, swapchain, backend resources
virtual void BeginRender() = 0;                          // acquire next frame
virtual void EndRender() = 0;                             // submit and present
virtual void Cleanup() = 0;
virtual void WaitIdle() {}                                // blocks until GPU work drains
virtual void FlushPendingTransfers() {}                    // flushes buffered uploads/staging
virtual void Resize(uint32_t width, uint32_t height) = 0;
virtual bool CreateImGUI() = 0;
virtual RenderContext* GetContext() = 0;
virtual uint32_t GetFeatureCaps() const { return CapShadows; }  // overridden per backend
```

plus concrete getters/setters for post-process toggles (SSAO, SSR, TAA,
bloom, tonemapping, IBL, shadow map resolution, MSAA, VSync). There is no
`RenderFrame()` method; the frame boundary is `BeginRender()` /
`EndRender()`, with `RenderCommandQueue` replaying queued commands in
between (section 3).

`Sleak::RenderEngine::RendererFactory`
(`include/private/Graphics/Common/RendererFactory.hpp`) instantiates the
active backend:

```cpp
static Renderer* CreateRenderer(RendererType type, Window* window);
static Renderer* ParseArg(std::string arg, Window* window);  // "vulkan"/"v", "opengl"/"o", "d3d11", "d3d12", ...
```

`RendererType` is `Vulkan`, `OpenGL`, `DirectX11`, `DirectX12`. DirectX 11
and DirectX 12 are only compiled under `PLATFORM_WIN`; requesting DirectX 12
on a GPU that fails `DirectX12Renderer::IsSupport()` falls back to
`CreateRenderer(RendererType::DirectX11, window)` with a warning.

`Sleak::Application::Run(GameBase* game)` drives one frame:

```
CoreWindow->Update()                          // polls OS/input events
apply pending resize (deferred from the resize event handler)
renderer->BeginRender()
  while (accumulated fixed time remains)
    activeScene->FixedUpdate(1.0f / 60.0f)
  activeScene->Update(deltaTime)
  activeScene->LateUpdate(deltaTime)
  game->Loop(deltaTime)
  debug overlay render, benchmark tick
renderer->FlushPendingTransfers()
RenderCommandQueue::GetInstance()->ExecuteCommands(context)
renderer->EndRender()
```

Shutdown calls `renderer->WaitIdle()` (also exposed as
`Application::WaitGPUIdle()`) before any scene teardown, since descriptor
sets can still reference sampler/image-view resources owned by scene
objects; then deletes the `GameBase` (cascading into scene and object
cleanup), `Sleak::UI::ShutdownTextureCache()`, `MeshBatch::Shutdown()`, and
`renderer->Cleanup()`.

---

## 2. Per-Backend Feature Support

Each backend reports what it actually implements through an overridable
`GetFeatureCaps()` bitmask (`GraphicsCaps`: `CapDeferred`, `CapSSAO`,
`CapSSR`, `CapTAA`, `CapBloom`, `CapIBL`, `CapTonemapPass`, `CapShadows`,
`CapHDRTarget`, `CapVelocity`, `CapLightShaft`, `CapProceduralSky`). The
base class defaults to `CapShadows` only.

| Backend | `GetFeatureCaps()` returns | Approx. source size |
| :--- | :--- | :--- |
| Vulkan | `CapDeferred \| CapSSAO \| CapSSR \| CapTAA \| CapBloom \| CapIBL \| CapShadows \| CapHDRTarget` | ~12,200 lines / 16 files (`VulkanBloom`, `VulkanDeferred`, `VulkanIBL`, `VulkanShadow`, `VulkanSSAO`, `VulkanSSR`, `VulkanTAA`, ...) |
| OpenGL | `CapDeferred \| CapSSAO \| CapIBL \| CapShadows` | ~2,400 lines / 6 files (no dedicated SSR/TAA/bloom files) |
| DirectX 11 | `CapShadows \| CapTonemapPass` | ~2,400 lines / 5 files |
| DirectX 12 | `CapShadows` (base default, not overridden further) | ~3,000 lines / 5 files |

Vulkan is the most feature-complete backend by a wide margin. Per-backend
mechanics:

- **Vulkan**: SPIR-V shaders (`.spv`), descriptor sets and push constants.
  Command submission is single-threaded: one `VkCommandPool` on
  `VulkanRenderer`, plus a second static pool used only for staging/upload
  batching, not per-thread recording.
- **DirectX 11**: constant buffers and input layouts, a single
  `ID3D11DeviceContext` (immediate context only, no deferred contexts).
- **DirectX 12**: root signatures, several descriptor heaps (RTV, DSV,
  shadow DSV, ImGui SRV, shared SRV), per-frame command allocators, and a
  single `ID3D12GraphicsCommandList`.
- **OpenGL**: GLSL shaders, vertex array objects, a single `SDL_GLContext`
  (single-threaded, no secondary/shared context).

---

## 3. `RenderCommandQueue`

`Sleak::RenderEngine::RenderCommandQueue`
(`include/private/Graphics/Common/RenderCommandQueue.hpp`) is a singleton,
frame-scoped queue of recorded draw/state commands, replayed into a
`RenderContext` at flush time:

```cpp
void SubmitDrawIndexed(RefPtr<BufferBase> vertexBuffer, RefPtr<BufferBase> indexBuffer,
                        List<RefPtr<BufferBase>> constantBuffers, uint32_t indexCount,
                        uint32_t startIndexLocation = 0, int32_t baseVertexLocation = 0,
                        bool castsShadow = true);
void SubmitDraw(RefPtr<BufferBase> vertexBuffer, List<RefPtr<BufferBase>> constantBuffers,
                 uint32_t vertexCount, uint32_t startVertexLocation = 0);
void SubmitBindConstantBuffer(RefPtr<BufferBase> buffer, uint8_t slot);
void SubmitUpdateConstantBuffer(RefPtr<BufferBase> buffer, void* Data, uint16_t Size);
void SubmitBindMaterial(Material* material);
void SubmitSetRenderMode(RenderMode mode);
void SubmitSetRenderFace(RenderFace face);
void SubmitCustomCommand(CustomCommand::ExecuteFunction function);

void ExecuteCommands(RenderContext* context);   // deferred: geometry pass, then lighting pass, then forward pass
                                                  // forward mode: all commands in submission order
void ExecuteShadowPass(RenderContext* context);
void Clear();     // drops this frame's queued commands, keeps the cached shadow draw list
void ClearAll();  // drops queued commands and the cached shadow draw list

static RenderCommandQueue* GetInstance();
```

Game code (through `MeshComponent`/`MeshBatch`) submits draws into the
queue during `Scene::Update`; `Application::Run` flushes it once per frame
after `FlushPendingTransfers()`. The queue also caches shadow-pass draws
across frames (`ShadowDrawEntry`, a 3-frame retirement window) so static
geometry does not need to resubmit into the shadow pass every frame.

---

## 4. Shared Resource Types

- **`Sleak::RenderEngine::Shader`** (`include/private/Graphics/Common/Shader.hpp`):
  an abstract compiled-shader handle with `compile(const std::string& path)`,
  `compile(const std::string& vert, const std::string& frag)`, and `bind()`.
  It has no separate geometry/compute-stage API.
- **`Sleak::RenderEngine::BufferBase`** (`include/private/Graphics/Common/BufferBase.hpp`):
  a GPU buffer abstraction over `BufferType::{Vertex, Index, Constant,
  ShaderResource, DepthStencil, RenderTarget, UnorderedAccess}`, extending
  `ResourceBase` and carrying a vertex-format tag used by
  @ref vertex_format "vertex format registration".
- **`Sleak::Texture`** (`include/public/Runtime/Texture.hpp`): one class
  covering `TextureType::{Texture2D, TextureCube, Texture3D}`, not a
  separate class per texture kind.
- **`Sleak::CubemapTexture`**: implemented separately per backend
  (`VulkanCubemapTexture`, `OpenGLCubemapTexture`,
  `DirectX11CubemapTexture`, `DirectX12CubemapTexture`).

---

## 5. The Deferred Split in Detail

On Vulkan, the deferred path is not a sequence of top-level calls from
`Application`. Most of it hangs off three points: `BeginRender`, the queue
flush, and `EndRender`.

`BeginRender` does two things before any game draw exists this frame. It
replays the cached shadow draw list if there is one, then opens the GBuffer
render pass and binds the GBuffer pipeline. Because `BeginRender` runs
before the scene update, the shadow pass necessarily replays the previous
frame's geometry, which is what the queue's cross-frame shadow cache is for.

`ExecuteCommands` then walks this frame's queue. Opaque, non-skinned draws
execute immediately into the open GBuffer pass. Draws whose material reports
`IsForwardRendered()`, skinned draws, and every custom command (skybox,
debug lines) are held back in a forward list instead.

`ExecuteDeferredLightingPass` closes the GBuffer pass, runs SSAO and its
bilateral blur, refreshes the GBuffer descriptors, then opens the lighting
pass and shades the whole screen with a single fullscreen triangle. SSAO
lives inside the lighting stage rather than in the post chain, since the
lighting pass consumes its result directly.

The forward transparent pass follows, replaying the held-back list against
the lit result so transparent surfaces, skinned meshes, and debug geometry
composite over it.

`EndRender` runs the post chain in a fixed order: TAA, then SSR, then
bloom, then the bloom composite, which also applies ACES tonemapping and
gamma and draws the ImGui layer straight into the swapchain image. TAA
running ahead of SSR is deliberate; TAA also issues the depth barrier SSR
would otherwise need. The command buffer closes, submits, and presents.

Backends without `CapDeferred` skip the split entirely and execute the queue
in submission order.

---

## 6. Where to Look in the Source

| Question | File |
| :--- | :--- |
| The frame loop and shutdown order | `src/Core/Application.cpp` |
| The lifecycle contract every backend implements | `include/private/Graphics/Common/Renderer.hpp` |
| Backend selection and the DirectX 12 fallback | `include/private/Graphics/Common/RendererFactory.hpp` |
| Queue commands, replay, and the shadow cache | `src/Graphics/Common/RenderCommandQueue.cpp` |
| Vulkan frame boundary and post chain | `src/Graphics/Vulkan/VulkanRenderer.cpp` |
| The GBuffer, SSAO, and lighting passes | `src/Graphics/Vulkan/VulkanDeferred.cpp`, `VulkanSSAO.cpp` |
| The OpenGL deferred path | `src/Graphics/OpenGL/OpenGLRenderer.cpp` |
