# Troubleshooting {#troubleshooting}

Failures in a four-backend engine tend to be quiet. A missing shader variant,
a rejected setting, or a stale staged asset produces no crash and often no
log line, just something that does not appear on screen. The entries below
are the ones worth checking first, each with the cause and the fix.

---

## 1. Startup and Assets

### Nothing loads, or the window opens onto a black screen and shader errors

**Cause.** Every asset path in the engine is relative and rooted at
`assets/`, resolved against the process working directory. Launching from
anywhere that is not the executable's own directory breaks all of them.

**Fix.** Set the working directory to the executable's directory as the first
statement in `main()`, before `CommandLine::Parse` and before constructing
`Sleak::Application`:

```cpp
std::filesystem::current_path(
    std::filesystem::path(argv[0]).parent_path().empty()
        ? std::filesystem::current_path()
        : std::filesystem::weakly_canonical(
              std::filesystem::path(argv[0])).parent_path());
```

If `argv[0]` has no parent path, which happens when the binary is found on
`PATH`, this leaves the working directory alone and asset loading still
fails. Run the binary by path.

### A shader or texture edit has no effect

**Cause.** The staged copy under the runtime directory is stale. A CMake
`POST_BUILD` asset copy fires only when the executable relinks, and editing
an asset changes no `.cpp`, so nothing relinks and nothing copies.

**Fix.** Track each asset as its own custom command output rather than using
a post-build directory copy, so edits propagate on every `cmake --build`. See
@ref assets_resources for the pattern. Deleting the staged `assets/` tree and
rebuilding is the quick confirmation that staleness is the problem.

### SPIR-V never recompiles after a GLSL edit

**Cause.** The engine's `CompileShaders` target only exists when CMake finds
`glslc`. Without it, configure prints
`glslc not found, SPIR-V shaders will not be auto-compiled` and the target is
never created, so every `.spv` stays at its committed revision.

**Fix.** Install the Vulkan SDK, or set `VULKAN_SDK` so `find_program` locates
`glslc`, then re-run CMake.

### A newly added engine shader is ignored

**Cause.** `CompileShaders` globs without `CONFIGURE_DEPENDS`, so the file
list is fixed at configure time. Edits to existing shaders are tracked; new
files are not.

**Fix.** Re-run CMake after adding a shader.

### Game shaders are never compiled at all

**Cause.** The engine's shader target is scoped to the engine's own
`assets/shaders` directory. A game's shader folder is invisible to it.

**Fix.** Give the project its own compile step. See @ref shader_authoring.

---

## 2. Rendering

### Geometry disappears on one backend only

**Cause.** A missing shader variant. Each backend rewrites a `.hlsl` path
into its own naming, and there is no fallback shader anywhere in the engine.

**Fix.** Grep for the stem and check all four variants exist. The DirectX
backends log only `Failed to compile vertex shader!` with no path attached,
so the log will not tell you which file was wanted. @ref shader_authoring has
the full mapping.

### Custom vertex format geometry does not draw

**Cause.** On Vulkan, a stem failed to compile or the format was rejected.
Compile failures log once, then the pass is marked permanently failed and
draws are dropped rather than retried. A handle that is unknown, a zero
stride, or an empty attribute list is rejected with **no log at all**. The
main variant gates everything: if `shaderStem` fails, the format is unbound
in every pass, including shadow and gbuffer variants that compiled fine.

**Fix.** Read the log at startup, before the first frame, for
`Failed to compile '{path}' for vertex format {n}`. If the log is empty,
check that `VertexFormatRegistry::Register` actually ran and that `stride`
and `attributes` are populated.

### Custom vertex format geometry renders as garbage on DirectX

**Cause.** Neither DirectX backend references the vertex format registry.
Custom-format buffers are interpreted through the built-in 96-byte
`Sleak::Vertex` layout.

**Fix.** Custom vertex formats work on Vulkan and OpenGL only. See
@ref backend_support.

### MSAA has no effect

**Cause.** `Renderer::SetMSAASampleCount` silently rejects any count above 1
while deferred rendering is enabled, because the GBuffer attachments are
single-sample. `m_deferredEnabled` defaults to true on the base renderer and
nothing ever clears it, so the check fires on every backend, including the
forward-only ones. There is no log and no return value, and
`GetMSAASampleCount()` keeps reporting the old value.

**Fix.** Treat MSAA as unavailable. Use TAA on Vulkan when you need
anti-aliasing.

### Nothing is shadowed on DirectX 12

**Cause.** DirectX 12 builds the shadow map every frame, but
`default_shader_dx12.hlsl` declares only a diffuse texture and sampler and
computes no shadow term. The map is rendered and discarded.

**Fix.** Use a different backend for shadowed content, or extend the DX12
shader. `GetGraphicsCaps()` reports `CapShadows` here, so do not rely on the
mask alone for this one.

### OpenGL falls back to forward and SSAO stops working

**Cause.** `CreateGBufferResources` compiles
`assets/shaders/lighting_pass_gl.frag`, which the engine does not ship. The
pass logs `Failed to compile deferred lighting pass shader!`, tears the
GBuffer down, and returns false. `CreateSSAOResources` then bails because it
requires a live GBuffer.

**Fix.** Supply `lighting_pass_gl.frag` in the project's own shader folder.
Game assets are staged over engine assets, so a game-side file of that name
is what the loader finds.

### `pcssEnabled` does nothing

**Cause.** The flag reaches the shader only on OpenGL and DirectX 11. Vulkan
and DirectX 12 never read it.

**Fix.** None needed; this is the current behavior. Vulkan's forward path
always runs PCSS and its deferred path always runs fixed-radius PCF.

### Changing the shadow map resolution at runtime does nothing

**Cause.** Once a backend has created its shadow resources,
`SetShadowMapResolution` queues the request into a pending field and returns.
Only Vulkan overrides `ApplyShadowResolutionChange()` to act on it. The other
three store the value and never apply it, without logging.

**Fix.** Set the resolution through `GraphicsConfig` before the renderer
creates shadow resources, or accept the default outside Vulkan.

### Point, spot, or area lights are invisible on Vulkan

**Cause.** The 16-light constant buffer is never bound on Vulkan;
`VulkanBuffer::Update()` is empty and nothing wires slot 2 into a descriptor
set. Vulkan lights from `ShadowLightUBO`, which carries one primary
directional light plus up to three extra directional lights.

**Fix.** Non-directional lights work on OpenGL and DirectX 11. On Vulkan,
compose with directional lights. See @ref lighting.

### Whole-scene shadow flicker when the camera rotates

**Cause.** The deferred lighting pass reconstructs world position from depth
using `InvViewProj`. A matrix built before the camera object updated is one
frame stale, and every reconstructed position is wrong.

**Fix.** `SceneBase::Update` already orders object updates before
`LightManager::UpdateAndBind` for exactly this reason. If you override
`Update`, preserve that order. `--shadowfreeze` latches the first light
view-projection forever, which separates a projection bug from a stability
bug.

---

## 3. Lifetime and Teardown

### The driver crashes when swapping scenes

**Cause.** Descriptor sets and command buffers still reference sampler and
image-view resources owned by the scene being torn down.

**Fix.** Flush the GPU before freeing anything:

```cpp
Sleak::Application::GetInstance()->WaitGPUIdle();
SetActiveScene(nextScene);
```

The engine does this itself at shutdown, before deleting the `GameBase`.
Runtime swaps are yours to guard.

### A crash on the frame after a scene is destroyed

**Cause.** Event handlers outlive the objects they were bound to.
`EventDispatcher::RegisterEventHandler` binds a raw `this` and returns a
string id; if the id is never unregistered, the next dispatch calls into
freed memory.

**Fix.** Keep every id and release it in the scene's `OnDeactivate` and
destructor:

```cpp
void WorldScene::OnDeactivate() {
    if (!m_keyId.empty()) {
        Sleak::EventDispatcher::UnregisterEvent(
            Sleak::EventType::KeyPressed, m_keyId);
        m_keyId.clear();
    }
    Sleak::Scene::OnDeactivate();
}
```

`UnregisterEvents(type)` drops every handler for one type and
`UnregisterAllEvents()` drops all of them, which are blunter but useful at
shutdown.

### Double free after handing a raw pointer to two owners

**Cause.** `Sleak::RefPtr` allocates a separate control block per raw-pointer
construction. Two `RefPtr`s built from the same raw pointer each believe they
are the sole owner.

**Fix.** Construct the `RefPtr` once and copy the `RefPtr`, never the raw
pointer. The same applies to `Sleak::Texture`: a material owns its textures
and frees them, so do not share one across materials.

### A GameObject deleted directly corrupts the scene

**Cause.** The scene owns its objects and tracks them in a list.

**Fix.** Call `SceneBase::DestroyObject`, which defers the delete to the end
of the frame, rather than `delete`.

### `WeakPtr` will not compile

**Cause.** `include/public/Memory/WeakPtr.hpp` references members `RefPtr`
does not have. Nothing in the engine instantiates it, which is why it still
builds.

**Fix.** Do not use it. There is no weak-observation option for engine
resources today.

---

## 4. Graphics Validation

### A `VK_ERROR_DEVICE_LOST` with nothing in the log

**Cause.** The Vulkan validation layer is not installed. The engine enables
it unconditionally when present and logs
`Vulkan validation layer enabled`. When the layer is absent it warns that
`VK_LAYER_KHRONOS_validation` is unavailable and GPU errors will go
unreported, drops `VK_EXT_debug_utils`, and continues.

**Fix.** Install the Vulkan SDK. There is no flag to turn validation on;
availability is the only switch. A game advertising its own `--validate`
option is describing behavior the engine already performs.

---

## 5. Animation

### An imported model does not animate

**Cause.** Any of several. The animator is attached only when the import
found bones and at least one clip. It lives on a per-mesh **child**, not on
the root object the loader returns. It also needs a sibling
`Sleak::MeshComponent` and logs a warning when there is none.

**Fix.** Walk `GetChildren()` to find the `Sleak::AnimatorComponent`, and
check the log for the missing-mesh warning.

### `Play()` is ignored

**Cause.** Once `CreateStateMachine()` has been called, `Update` consults the
machine and returns before the single-clip path.

**Fix.** Drive playback through state machine parameters, not `Play`.

### Skeletal animation does nothing on DirectX

**Cause.** `RenderContext::BindBoneBuffer` is a base no-op and neither
DirectX backend overrides it. `skinned_shader.hlsl` exists; the C++ plumbing
behind it does not.

**Fix.** Skinning runs on Vulkan and OpenGL.

### A blend divides by zero or freezes

**Cause.** `AddTransition` accepts a `blendDuration` of zero, which the
elapsed-time division then hits. A clip with a duration of zero hits the same
problem in the loop wrap. Transitions are also not interruptible: while a
blend is in flight no transition is evaluated.

**Fix.** Use a small positive blend duration for an instant cut, and keep
durations short so the non-interruptible window stays brief.

---

## 6. Reading the Logs

Most of the above is diagnosed from the log, not the screen. Raise the
verbosity mentally: `SLEAK_LOG` is trace and `SLEAK_FATAL` is critical, and
the engine logs pipeline and resource failures at error level with enough
context to name the subsystem. Startup is where nearly every quiet failure
announces itself exactly once, so read the first hundred lines before
debugging the picture. See @ref debug_tools.

---

## 7. Where to Look in the Source

| Question | File |
| :--- | :--- |
| GPU flush before teardown | `Application::WaitGPUIdle` in `src/Core/Application.cpp` |
| Handler registration and removal | `include/public/Events/Event.hpp` |
| MSAA rejection and shadow resolution queueing | `include/private/Graphics/Common/Renderer.hpp` |
| Custom format compile failures | `src/Graphics/Vulkan/VulkanPipelines.cpp` |
| OpenGL GBuffer teardown on shader failure | `src/Graphics/OpenGL/OpenGLRenderer.cpp` |
| Validation layer selection | `src/Graphics/Vulkan/VulkanDevice.cpp` |
| Update ordering and the stale-matrix comment | `SceneBase::Update` in `src/Scene/SceneBase.cpp` |
| Reference counting rules | `include/public/Memory/RefPtr.hpp` |
