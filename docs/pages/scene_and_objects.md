# Scenes, Objects, and Components {#scene_and_objects}

This page describes the application/game/scene ownership chain, the
GameObject/Component object model, and smart pointer ownership.

| Type | Role |
| :--- | :--- |
| `Sleak::Application` | Owns the window, the renderer, and the game loop; one per process. |
| `Sleak::GameBase` | Your game's top-level class; owns the scene registry and the active scene. |
| `Sleak::SceneBase` | Where scene behavior lives: object list, state machine, lights, physics world, camera. |
| `Sleak::Scene` | Thin `SceneBase` subclass adding the `bEnableFixedUpdate` toggle. |
| `Sleak::GameObject` | Named entity with a tag, an optional parent/child link, and attached components. |
| `Sleak::Component` | Base class for attached behavior with its own update hooks. |
| `Sleak::RefPtr<T>` | The engine's owning pointer, used in place of `std::shared_ptr`. |

\dot
digraph ownership {
  bgcolor="transparent"; rankdir=TB;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];
  app  [label="Application\nwindow, renderer, loop"];
  game [label="GameBase\nscene registry"];
  s1   [label="SceneBase (active)", fillcolor="#22d3ee22", color="#22d3ee"];
  s2   [label="SceneBase (inactive)"];
  go1  [label="GameObject"];
  go2  [label="GameObject (child)"];
  c1   [label="TransformComponent"];
  c2   [label="MeshComponent"];
  c3   [label="RigidbodyComponent"];
  app  -> game [label="Run(game)"];
  game -> s1 [label="SetActiveScene"];
  game -> s2;
  s1 -> go1 [label="AddObject"];
  go1 -> go2 [label="SetParent"];
  go1 -> c1 [label="AddComponent<T>"];
  go1 -> c2;
  go1 -> c3;
}
\enddot

Each arrow is real ownership. Deleting a level of the tree destroys
everything below it, which is why objects are never deleted by caller code.

Per frame, `Application::Run` calls into the active scene, which fans the
call out to its objects and their components in this order:

\dot
digraph lifecycle {
  bgcolor="transparent"; rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];
  load  [label="Load()\nOnLoad + Initialize"];
  act   [label="Activate()\nOnActivate"];
  begin [label="Begin()"];
  fixed [label="FixedUpdate(1/60)\nzero or more times", fillcolor="#22d3ee22", color="#22d3ee"];
  upd   [label="Update(dt)", fillcolor="#22d3ee22", color="#22d3ee"];
  late  [label="LateUpdate(dt)", fillcolor="#22d3ee22", color="#22d3ee"];
  deact [label="Deactivate()\nOnDeactivate"];
  unl   [label="Unload()\nOnUnload + destroy objects"];
  load -> act -> begin -> fixed -> upd -> late;
  late -> fixed [label="next frame", style=dashed];
  late -> deact [label="scene swap"];
  deact -> unl;
}
\enddot

The three cyan hooks run every frame. Everything else runs once per
load/activate cycle.

---

## 1. Application, GameBase, and Scenes

`Sleak::Application` (`include/public/Core/Application.hpp`) owns the
window, the active renderer, and the game loop; one instance exists per
process. `Sleak::CommandLine` is parsed once in `main()` before an
`Application` is constructed. `Application::Run(GameBase* game)` drives the
loop described in @ref rendering_pipeline "the rendering pipeline page".

`Sleak::GameBase` (`include/public/Core/GameBase.hpp`) is the class a
game's top-level object derives from. It owns the scene registry and the
active scene:

```cpp
virtual void AddScene(SceneBase* scene);        // takes ownership
virtual void RemoveScene(SceneBase* scene);      // unloads, deletes, drops from registry
virtual void SetActiveScene(SceneBase* scene);   // deactivates current, activates given
virtual SceneBase* GetActiveScene();
```

`Application` only drives whichever `GameBase*` it is given; `GameBase`'s
own `Initialize()`, `Begin()`, and `Loop(deltaTime)` are pure virtual and
implemented by the game.

`Sleak::Scene` (`include/public/Core/Scene.hpp`) is a thin subclass of
`Sleak::SceneBase` (`include/public/Core/SceneBase.hpp`) that adds one
toggle, `bEnableFixedUpdate`. `SceneBase` is where scene behavior actually
lives:

```cpp
virtual bool Initialize();                 // once, before the first Update
virtual void Begin() = 0;                   // activates every owned object
virtual void Update(float deltaTime) = 0;
virtual void FixedUpdate(float fixedDeltaTime);
virtual void LateUpdate(float deltaTime);

void Load();    // Unloaded -> Loading -> Active, calls OnLoad() + Initialize()
void Unload();  // deactivates, calls OnUnload(), destroys all owned objects
void Activate();
void Deactivate();
void Pause();
void Resume();

virtual void AddObject(GameObject* object);      // scene takes ownership
virtual void RemoveObject(GameObject* object);    // unregisters and deletes immediately
void DestroyObject(GameObject* object);            // queues; freed on the next ProcessPendingDestroy()
```

`SceneBase` owns the object list, a `SceneState` machine (`Unloaded`,
`Loading`, `Active`, `Paused`, `Unloading`), a `LightManager*`, a
`Physics::PhysicsWorld*` (see @ref physics), a `Skybox*`, and the active
camera. It does not itself perform render-pass submission or frustum
culling; those are handled by the renderer and @ref culling "the culling
system" respectively.

---

## 2. GameObject and Component

`Sleak::GameObject` (`include/public/Core/GameObject.hpp`) is the base
entity type. A `GameObject` does not automatically receive a
`TransformComponent`: one must be added explicitly with
`AddComponent<TransformComponent>(...)`, as the static factory helpers
(`GameObject::CreatePlane`, `CreateCube`, `CreateSphere`, ...,
`src/Scene/GameObject.cpp`) do.

```cpp
template<typename T, typename... Args> void AddComponent(Args&&... args);
template<typename T> void RemoveComponent();
template<typename T> T* GetComponent();
template<typename T> bool HasComponent();

void SetParent(GameObject* parent);
GameObject* GetParent() const;
const List<GameObject*>& GetChildren() const;
void MarkForDestroy();  // deferred destruction on the next scene pass
```

`Sleak::Component` (`include/public/ECS/Component.hpp`) is the base class
for attached behavior:

```cpp
virtual bool Initialize() = 0;
virtual void Update(float deltaTime) = 0;
virtual void FixedUpdate(float fixedDeltaTime) {}
virtual void LateUpdate(float deltaTime) {}
virtual void OnDestroy() {}
virtual void OnEnable() {}
virtual void OnDisable() {}
GameObject* GetOwner();
```

Real components include `TransformComponent`, `MeshComponent`,
`MaterialComponent`, `AnimatorComponent` (all under
`include/public/ECS/Components/`), the `CameraController` base with its
`FreeLookCameraController` and `FirstPersonController` subclasses, and
`RigidbodyComponent` / `ColliderComponent` (which live under
`include/public/Physics/`, not `ECS/Components/`).

Although the headers live under a folder named `ECS/`, this is not a
data-oriented entity component system. Each `GameObject` owns its
components directly in a `List<RefPtr<Component>>`; components are
individually heap-allocated, `GetComponent<T>()` performs a linear
`dynamic_cast` scan, and updates dispatch through virtual calls rather than
a system iterating packed component arrays. It is best described as a
GameObject/Component object model, similar to Unity's original
MonoBehaviour composition.

---

## 3. Smart Pointers (`Sleak::RefPtr`)

- **`Sleak::RefPtr<T>`** (`include/public/Memory/RefPtr.hpp`): the engine's
  standard owning pointer, used in place of `std::shared_ptr`. It is
  reference-counted with an atomic count, but the count lives in a
  separately heap-allocated `SharedControlBlock`, not inside `T` itself,
  making it functionally closer to a hand-rolled `shared_ptr` than to
  classic intrusive reference counting.
- **`Sleak::ObjectPtr<T>`** (`include/public/Memory/ObjectPtr.hpp`) is
  move-only and exclusively owning (a `unique_ptr` equivalent, not a
  non-owning tracking pointer): its destructor deletes the owned object and
  copying is disabled.
- **`Sleak::WeakPtr<T>`** (`include/public/Memory/WeakPtr.hpp`) is intended
  as a non-owning observer of a `RefPtr<T>`, but its constructor reads a
  `refCount` member that `RefPtr` does not expose (`RefPtr` exposes
  `controlBlock` instead), and it has no call sites anywhere in the engine.
  Treat it as unused, not as a supported pattern.

`GameObject` instances are deleted only from within `Scene`.
`SceneBase::DestroyObject()` defers deletion: it marks the object and its
children with `MarkForDestroy()` and deletes them later in
`ProcessPendingDestroy()`. `SceneBase::RemoveObject()` and
`DestroyAllObjects()` / `~SceneBase()` delete objects immediately instead.
Nothing in the engine prevents calling `delete` on a `GameObject` directly,
since `GameObject`'s destructor is public, but by convention destruction
always goes through one of `Scene`'s own paths rather than caller code.

---

## 4. Ordering Details Worth Knowing

`OnActivate` runs before `Begin`, not after. `GameBase::SetActiveScene`
deactivates the outgoing scene, activates the incoming one, initializes it
if this is its first activation, fires `OnActivate`, and only then calls
`Begin`. Anything `Begin` depends on has to be ready by `OnActivate`.

`Begin` is what actually switches objects on. It walks the scene's object
list calling `SetActive(true)`, which is the call that fires
`Component::OnEnable`. There is no `Start` hook anywhere in the engine.

Inside `SceneBase::Update`, objects tick before the rest of the scene. The
order is objects, then light manager bind, then skybox, then the physics
step, then debug line flush, then deferred destruction. Object updates come
first on purpose so camera matrices are current before anything reads them.

`FixedUpdate` and `LateUpdate` iterate only root objects, and each
`GameObject` recurses into its own active children. An object parented to an
inactive parent stops receiving updates even if it is active itself.
`Scene::FixedUpdate` returns immediately unless `bEnableFixedUpdate` is set,
so the fixed path is opt-in per scene.

---

## 5. Where to Look in the Source

| Question | File |
| :--- | :--- |
| Scene state transitions and the update order | `src/Scene/SceneBase.cpp` |
| Scene registry and activation order | `src/Core/GameBase.cpp` |
| Startup, the frame loop, and shutdown | `src/Core/Application.cpp` |
| Component storage, hierarchy, and factories | `src/Scene/GameObject.cpp` |
| The component hook contract | `include/public/ECS/Component.hpp` |
| Reference counting and the control block | `include/public/Memory/RefPtr.hpp` |
