\htmlonly
<div class="se-hero">
<img src="logo-256.png" alt="SleakEngine"/>
<h1>SleakEngine</h1>
<p>A C++23 game engine with four graphics backends. Write your game class, register your scenes, and the engine runs the window, rendering, lighting, physics, culling, and input.</p>
<div><a class="se-btn se-btn-primary" href="getting_started.html">Get Started</a> <a class="se-btn se-btn-secondary" href="topics.html">API Reference</a></div>
</div>
<div class="se-cards">
<a class="se-card" href="getting_started.html"><b>Getting Started</b><span>From an empty directory to a running window with a lit, textured object you can fly around.</span></a>
<a class="se-card" href="your_first_scene.html"><b>Your First Scene</b><span>Model loading, lighting, input handling, physics bodies, and per-frame UI.</span></a>
<a class="se-card" href="rendering_pipeline.html"><b>Rendering Pipeline</b><span>The Renderer lifecycle, four backends, and how queued draws become a frame.</span></a>
<a class="se-card" href="vertex_format.html"><b>Vertex Formats</b><span>Register your own vertex layouts and shaders at runtime; the engine builds the pipelines.</span></a>
<a class="se-card" href="scene_and_objects.html"><b>Scenes and Objects</b><span>Application lifecycle, GameObjects, components, and RefPtr ownership.</span></a>
<a class="se-card" href="events_and_input.html"><b>Events and Input</b><span>The EventDispatcher, event types, and the input handling patterns games use.</span></a>
<a class="se-card" href="physics.html"><b>Physics</b><span>PhysicsWorld stepping, rigid bodies, collider shapes, and the AABB broadphase.</span></a>
<a class="se-card" href="culling.html"><b>Culling</b><span>CPU frustum and software occlusion culling, wired automatically per camera.</span></a>
</div>
\endhtmlonly

<h2>How it fits together</h2>

Your game is one class. Everything below it is engine, and the layer
boundaries are real: game code never reaches past the subsystem API, and the
subsystems never name a specific backend.

\dot
digraph layers {
  bgcolor="transparent"; rankdir=TB; compound=true;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];

  game [label="Your game\nGameBase, your Scenes, your Components", fillcolor="#22d3ee22", color="#22d3ee"];

  subgraph cluster_sub {
    label="Engine subsystems"; color="#3b82f6"; fontcolor="#7aa7d9";
    fontname="Helvetica"; fontsize=11; style=rounded;
    scene   [label="Scene\nGameObject, Component"];
    physics [label="Physics\nPhysicsWorld, DynamicAABBTree"];
    events  [label="Events\nEventDispatcher"];
    culling [label="Culling\nCullingSystem"];
    ui      [label="UI\npanels, text, 2D draw"];
  }

  queue [label="RenderCommandQueue\nqueued draws for the frame"];
  rend  [label="RenderEngine::Renderer\nbackend-agnostic lifecycle", fillcolor="#22d3ee22", color="#22d3ee"];

  subgraph cluster_backends {
    label="Backends"; color="#3b82f6"; fontcolor="#7aa7d9";
    fontname="Helvetica"; fontsize=11; style=rounded;
    vk  [label="VulkanRenderer"];
    gl  [label="OpenGLRenderer"];
    d11 [label="DirectX11Renderer"];
    d12 [label="DirectX12Renderer"];
  }

  gpu [label="GPU", shape=box, fillcolor="#22d3ee22", color="#22d3ee"];

  game -> scene;
  game -> physics;
  game -> events;
  game -> culling;
  game -> ui;
  scene -> culling [label="IsVisible before drawing"];
  scene -> queue [label="submit draws"];
  queue -> rend [label="ExecuteCommands, replayed\nbetween BeginRender and EndRender"];
  rend -> vk;
  rend -> gl;
  rend -> d11;
  rend -> d12;
  vk  -> gpu;
  gl  -> gpu;
  d11 -> gpu;
  d12 -> gpu;
}
\enddot

You pick the backend at launch; nothing above the `Renderer` line changes
when you do.

<h2>What you get</h2>

**Four graphics backends behind one API.** Vulkan, OpenGL, DirectX 11, and
DirectX 12 all implement the same `Sleak::RenderEngine::Renderer`
lifecycle. You pick one at launch with `-r vulkan|opengl|d3d11|d3d12`, or
take the platform default: DirectX 11 on Windows, Vulkan everywhere else.
Feature support is uneven by design. Vulkan is the most complete path
(deferred shading, SSAO, SSR, TAA, bloom, image-based lighting), OpenGL
covers deferred shading with SSAO and IBL, and the DirectX backends
currently expose shadows plus tonemapping on DX11. Query what the running
backend actually supports with `Application::GetGraphicsCaps()`.

**A GameObject and Component object model.** `Sleak::GameObject` is a named
entity with a tag, an optional parent/child hierarchy, and any number of
attached components. `AddComponent<T>()` constructs a component in place;
`GetComponent<T>()` finds it. Transform, mesh, material, animator, and
camera-controller components ship with the engine, and your own components
just derive from `Sleak::Component`.

**Scenes with a real lifecycle.** Subclass `Sleak::Scene` and override the
hooks you need: `OnLoad` and `OnUnload` for assets, `OnActivate` and
`OnDeactivate` for entering and leaving, `Begin` for one-time setup, and
`Update` / `FixedUpdate` / `LateUpdate` for per-frame work. The scene owns
every object you add to it and destroys them on unload.

**Physics that follows your colliders.** Attach a `ColliderComponent` and a
`RigidbodyComponent` and the scene's `Sleak::Physics::PhysicsWorld` picks
them up automatically. It maintains a dynamic AABB broadphase tree and
answers raycast, sphere-sweep, and overlap queries against everything
registered in it.

**Culling that costs no GPU time.** `Sleak::CullingSystem` does view-frustum
culling plus software occlusion culling on the CPU, rasterizing occluder
volumes you submit into a small depth buffer. It adapts: when a frame culls
nothing, it stops rasterizing and probes again later.

**Runtime vertex formats.** The engine ships no game-specific vocabulary.
Describe your own vertex layout with `Sleak::VertexLayoutDesc`, register it
through `Sleak::VertexFormatRegistry::Register`, and build meshes against
the returned handle with `Sleak::MeshBatch::CreateMesh`. The renderer binds
the matching pipeline off the handle. See
@ref vertex_format "Vertex Format Registration".

**Events you subscribe to by member function.**
`Sleak::EventDispatcher::RegisterEventHandler(this, &MyScene::OnKeyPressed)`
is the whole registration story. Window, keyboard, and mouse events are
dispatched synchronously to everyone listening for that type.

**A UI wrapper for HUDs and tools.** `Sleak::UI` gives you panels, text,
buttons, sliders, and 2D drawing without exposing Dear ImGui, which stays a
private engine dependency.

**Reference-counted resource ownership.** `Sleak::RefPtr<T>` is the owning
pointer used throughout the public API in place of `std::shared_ptr`.

<h2>Subsystem guides</h2>

- @ref rendering_pipeline "Rendering Pipeline": the `Sleak::RenderEngine::Renderer` base class, its four backend implementations, and how `RenderCommandQueue` turns queued draws into a frame.
- @ref vertex_format "Vertex Format Registration": `Sleak::VertexLayoutDesc`, `Sleak::VertexFormatRegistry`, and a worked example of registering and drawing a custom vertex format.
- @ref scene_and_objects "Scenes, Objects, and Components": `Sleak::Application`, `Sleak::GameBase`, `Sleak::Scene` / `Sleak::SceneBase`, `Sleak::GameObject`, `Sleak::Component`, and `Sleak::RefPtr` ownership.
- @ref events_and_input "Events and Input": `Sleak::EventDispatcher`, the `Sleak::Event` hierarchy, and the state of the engine's input-polling code.
- @ref physics "Physics and Spatial Partitioning": `Sleak::Physics::PhysicsWorld`, `Sleak::Physics::DynamicAABBTree`, and the collider/rigidbody components.
- @ref culling "Culling": `Sleak::CullingSystem`, the CPU frustum and software-occlusion visibility system.

<h2>API reference</h2>

Browse the [Topics](topics.html) list for the API grouped by subsystem
(Core, Scene, Rendering, Lighting, Physics, Culling, Math, Memory, Events,
Input, UI, Animation, Vertex Formats, Debug, Utility, File System), or the
[Namespaces](namespaces.html) and [Classes](annotated.html) lists for a
flat index.

<h2>Platforms and toolchain</h2>

- **Language**: C++23. **Build**: CMake 3.31 or newer.
- **Linux**: Vulkan and OpenGL. Vulkan is the default backend.
- **Windows**: DirectX 11, DirectX 12, Vulkan, and OpenGL. DirectX 11 is
  the default backend, and DirectX 12 falls back to DirectX 11 when the GPU
  does not support it.
- Dependencies are vendored (SDL3, glm, fmt, spdlog, assimp, Dear ImGui,
  stb, glad, json, yaml-cpp, and the Vulkan SDK), so there is nothing to
  install system-wide.

<h2>How the source is organized</h2>

```
include/
├── public/                  # The API your game includes, namespace Sleak
│   ├── Camera/               Camera, ViewFrustum
│   ├── Core/                 Application, Scene/SceneBase, GameObject, GameBase, Logger, CommandLine
│   ├── Culling/              CullingSystem
│   ├── Debug/                Benchmark, DebugOverlay, DebugLineRenderer, SystemMetrics
│   ├── ECS/                  Component, Components/ (Transform, Mesh, Material, Animator, camera controllers)
│   ├── Events/               Event, Delegate, EventDispatcher, ApplicationEvent, KeyboardEvent, MouseEvent
│   ├── FileSystem/           Serializable
│   ├── Input/                Keyboard, KeyCodes, InputManager
│   ├── Lighting/             LightManager, Directional/Point/Spot/Area lights
│   ├── Math/                 Vector, Matrix, Quaternion, Color, AABB, Random
│   ├── Memory/               RefPtr, ObjectPtr, WeakPtr, SmartPointer
│   ├── Physics/              PhysicsWorld, DynamicAABBTree, Colliders, RigidbodyComponent, ColliderComponent
│   ├── Runtime/              VertexLayout, MeshBatch, MeshData, Texture, Material, ModelLoader, Skeleton, AnimationClip
│   └── UI/                   UI panel wrapper
│
└── private/                 # Backend internals; not part of the API you code against
    ├── Core/                 Window
    ├── Assets/               InternalGeometry
    └── Graphics/
        ├── Common/           Renderer, RendererFactory, RenderContext, Shader, BufferBase, RenderCommandQueue
        ├── Vulkan/
        ├── DirectX11/
        ├── DirectX12/
        └── OpenGL/

src/                          # Implementation, mirrors include/
vendors/                      # Third-party libraries and the platform SDKs
```

Include only from `include/public/`. Everything under `include/private/` is
free to change between versions.

<h2>Made with SleakEngine</h2>

![SleakCraft, a voxel sandbox running on the Vulkan backend](sleakcraft_hero.jpg)

<h2>Shipped consumers</h2>

Two projects build on SleakEngine and are useful as worked examples:
SleakCraft, a voxel sandbox that registers its own 48-byte vertex format,
and SleakSims, a character and animation showcase. SleakEngine-Empty is the
starter template the Getting Started guide is built from.
