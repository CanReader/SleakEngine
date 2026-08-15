# Release Notes {#release_notes}

This page records what each SleakEngine release contains. Entries are
written against the code that shipped in them, newest first.

---

## 1.0

The first documented release. It establishes the engine's public surface:
four rendering backends behind one abstraction, a scene and object model
with components, physics with a dynamic broadphase, CPU visibility
culling, a UI layer, and an asset pipeline that stages content next to the
binary.

### Rendering

Four backend implementations sit behind `RenderEngine::Renderer` and its
`RenderContext` command-recording interface, selected at startup by the
`-r` flag or by the platform default. `RenderEngine::RenderCommandQueue`
collects draws and state changes during the scene update and replays them
into the active context once per frame, caching shadow-pass draws across
frames so static geometry does not resubmit.

| Backend | Path | Post-process coverage |
|---|---|---|
| Vulkan | Deferred | SSAO, SSR, TAA, bloom, IBL, HDR target, shadows |
| OpenGL | Deferred | SSAO, IBL, shadows |
| DirectX 11 | Forward | Tonemap pass, shadows |
| DirectX 12 | Forward | Shadows |

Each backend reports what it implements through a capability mask, readable
from `Sleak::Application::GetGraphicsCaps()`, so settings UI can hide what
the running backend ignores. Vulkan is the reference implementation and the
only backend with the complete post chain.

`Sleak::GraphicsConfig` carries render settings as plain data, with
`Preset()` building `Off`, `Low`, `Medium`, `High`, and `Ultra` tiers and
`Sleak::Application::ApplyGraphicsConfig` pushing them to the renderer in
one call.

### Custom vertex format API

The engine carries no per-game vertex vocabulary.
`Sleak::VertexLayoutDesc` describes a stride, an attribute list, and up to
four shader stems for the forward, shadow, gbuffer, and transparent passes.
`Sleak::VertexFormatRegistry::Register` returns a `VertexFormatHandle`,
`MeshBatch::CreateMesh` builds GPU buffers keyed to that handle, and the
renderers bind the matching pipeline off the handle carried on the mesh. An
empty stem skips that pass for the format; a stem that fails to load logs
an error and skips the pass rather than falling back to a default shader.

### Scenes, objects, and components

`Sleak::Application` owns the window, the renderer, and the frame loop, and
drives a `Sleak::GameBase` that owns a registry of scenes. A
`Sleak::Scene` owns its `Sleak::GameObject`s, a `Sleak::LightManager`, and
a `Sleak::Physics::PhysicsWorld`, and registers added objects with both
automatically. Ownership is strict: the scene destroys everything it holds
on unload, and `DestroyObject` defers the delete to a safe point in the
frame.

Components in this release include `TransformComponent`, `MeshComponent`,
`MaterialComponent`, `AnimatorComponent`, `ColliderComponent`,
`RigidbodyComponent`, `FirstPersonController`, `FreeLookCameraController`,
and `CameraController`. Primitive helpers on `GameObject` build planes,
cubes, spheres, capsules, cylinders, and tori for prototyping.

### Physics

`Sleak::Physics::PhysicsWorld` integrates dynamic bodies, refreshes a
`DynamicAABBTree` broadphase, and finds and resolves overlapping pairs each
step. Collider shapes are AABB, sphere, capsule, and triangle mesh, the
last constructible straight from `MeshData`. Bodies are `Static`,
`Kinematic`, or `Dynamic`, each with its own gravity setting.

The query API covers `Raycast`, `SphereSweep`, `OverlapSphere`, and
`OverlapAABB`, all filtered by an optional layer mask matched against
`ColliderComponent::GetLayer()`. Colliders can be flagged as triggers to
join the broadphase without generating a physics response.

### Culling

`Sleak::CullingSystem` is CPU only and backend agnostic: view-frustum
culling always available, plus software occlusion culling that rasterizes
game-submitted occluder volumes into a low-resolution depth buffer. The
occlusion pass is adaptive, standing down and probing on an interval when a
rasterized frame culls nothing, and it reports per-frame counters through
`GetStats()`.

### Lighting and shadows

Directional, point, spot, and area lights, all deriving from `GameObject`
and managed by a per-scene `Sleak::LightManager` that also owns ambient
color and distance fog. Directional lights cast shadow maps with
configurable resolution, distance, frustum size, bias, and strength.

### Animation

Skeletal animation with `Sleak::Skeleton`, `Sleak::AnimationClip`,
`Sleak::AnimatorComponent`, and `Sleak::AnimationStateMachine`. Rigged
imports arrive with a skeleton and clips already attached, and clips from
separate files bind to an existing skeleton through
`ModelLoader::LoadAnimationsOnly`.

### UI

`Sleak::UI` is an immediate-mode wrapper over the vendored Dear ImGui:
panels, text, buttons, checkboxes, drag floats, combos, list boxes, child
regions, style push and pop, progress bars, and direct 2D drawing including
images. ImGui itself stays a private dependency of the engine target, so
game code never includes it.

### Assets and content

`Sleak::ModelLoader` imports models through Assimp with the OBJ, FBX, and
glTF importers enabled, returning a `GameObject` hierarchy with meshes,
materials, and rigs attached, and caching textures per load. Cubemap
skyboxes, PBR materials, and a texture abstraction covering 2D, cube, and
3D textures round out the runtime side. On the build side, CMake stages the
engine and game asset trees into one directory next to the executable, and
compiles GLSL to SPIR-V when `glslc` is available.

### Input, events, and configuration

A static `Sleak::EventDispatcher` delivers keyboard, mouse, and window
events to registered callbacks or member functions, returning an id to
unregister with. `Sleak::CommandLine` parses flags and values, covering
backend selection, window size and title, and fullscreen, and accepts game
defined flags.

### Debugging and measurement

`Sleak::Benchmark` records per-frame CSV sessions with frame time, FPS,
triangles, CPU, and RAM, and closes each file with a summary block carrying
percentiles, standard deviation, spike counts, and system information.
Games register their own metrics as extra columns. A debug overlay, a debug
line renderer, and system metrics collection ship alongside it, with
runtime hotkeys for camera toggle, fullscreen, and benchmark recording.

### Platforms and build

C++23, CMake 3.31 or newer, consumed as a source subproject with all
dependencies vendored. Linux is the primary platform, with Vulkan as the
default backend and OpenGL as the fallback. Windows adds DirectX 11, which
is its default, and DirectX 12.

### Known limitations in 1.0

- The DirectX backends run forward only. Deferred-path settings are
  accepted and ignored there.
- MSAA and the deferred path are mutually exclusive; the GBuffer is single
  sampled and the sample count is forced to 1.
- `GraphicsConfig::renderScale` is declared but not consumed by any
  backend.
- Shadow PCF tap counts are compile-time constants in the shaders, so the
  quality tiers change shadow map resolution rather than filter cost.
- `ApplyGraphicsConfig` forwards the renderer-owned fields only. Culling,
  shadow-light, and grading settings are applied by the game through their
  own systems.

---

Later releases will list their changes here, newest first, with the same
breakdown by subsystem.
