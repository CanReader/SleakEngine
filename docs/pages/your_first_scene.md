# Your First Scene {#your_first_scene}

@ref getting_started "Getting Started" got a window on screen. This guide
fills that window with real content: imported models, a controllable
character, collision, keyboard input, on-screen UI, and a second scene to
switch to.

Read it start to finish or jump to the section you need.

---

## Load a model

`Sleak::ModelLoader::Load` imports a model file through Assimp and returns
the root of a `Sleak::GameObject` hierarchy, with meshes, materials, and
(for rigged files) a skeleton and animation clips already attached. You own
the returned root: add it to the scene and the scene takes over.

```cpp
#include <Runtime/ModelLoader.hpp>

Sleak::ModelLoadOptions opts;
opts.scaleFactor = 0.01f;  // source is in centimeters, engine works in meters
opts.flipUVs     = true;
opts.position    = Sleak::Math::Vector3D(0.0f, 0.0f, 0.0f);

Sleak::GameObject* model =
    Sleak::ModelLoader::Load("assets/models/Mannequin.fbx", opts);
if (!model) {
    SLEAK_ERROR("Failed to load {}", "assets/models/Mannequin.fbx");
    return;
}
AddObject(model);
```

`ModelLoadOptions` also carries `rotation`, `flipNormals`, and
`flipWinding`. Reach for the flip flags when a model imports inside out or
with black faces: they are almost always an exporter handedness mismatch,
not a shading bug.

Textures are cached per load, so a model that reuses one texture across
twenty materials loads it once.

## Play an animation

A rigged import gets a `Sleak::AnimatorComponent`. Extra clips from
separate files attach to the same skeleton with
`ModelLoader::LoadAnimationsOnly`, which is the standard workflow when your
mesh and your animations ship as different files.

```cpp
#include <ECS/Components/AnimatorComponent.hpp>
#include <Runtime/AnimationClip.hpp>

auto* animator = model->GetComponent<Sleak::AnimatorComponent>();
if (animator) {
    Sleak::Skeleton* skeleton = animator->GetSkeleton();

    for (auto* clip :
         Sleak::ModelLoader::LoadAnimationsOnly("assets/animations/Walking.fbx",
                                                skeleton)) {
        if (!clip) continue;
        clip->name = "Walking";
        animator->AddClip(clip);
    }

    animator->Play("Walking", /*loop=*/true);
}
```

`Play` also takes an integer clip index, and `GetClips()` hands you the
full list if you want to build a clip picker.

---

## Give the player a body

The camera is a `Sleak::GameObject`, so it can carry a collider and a
rigidbody like anything else. That is how you get a first-person character
that falls, lands, and cannot walk through walls.

```cpp
#include <Camera/Camera.hpp>
#include <ECS/Components/FirstPersonController.hpp>
#include <Physics/ColliderComponent.hpp>
#include <Physics/RigidbodyComponent.hpp>

auto* cam = new Sleak::Camera("PlayerCamera",
                              Sleak::Math::Vector3D(8.0f, 70.0f, 8.0f),
                              60.0f, 0.1f, 1500.0f);
cam->SetDirection(Sleak::Math::Vector3D(0.0f, 0.0f, 1.0f));

cam->AddComponent<Sleak::FirstPersonController>();
cam->AddComponent<Sleak::ColliderComponent>(
    Sleak::Physics::BoundingSphere(Sleak::Math::Vector3D(0, 0, 0), 0.3f));
cam->AddComponent<Sleak::RigidbodyComponent>(Sleak::BodyType::Dynamic);

if (auto* rb = cam->GetComponent<Sleak::RigidbodyComponent>()) {
    rb->SetUseGravity(true);
    rb->SetGravity(Sleak::Math::Vector3D(0.0f, -32.0f, 0.0f));
}

AddObject(cam);
cam->Initialize();
SetActiveCamera(cam);
```

Order matters here. `AddObject` is what registers the collider with the
scene's `Sleak::Physics::PhysicsWorld`, walking the whole child hierarchy
as it goes, so add the object after its components are attached.

Pick the body type to match how the object moves:

| `Sleak::BodyType` | Behavior |
|---|---|
| `Static` | Never moves. Infinite mass. Level geometry. |
| `Kinematic` | Moves under your control, pushes others, is not pushed. |
| `Dynamic` | Mass-based response, affected by its own gravity setting. |

`FirstPersonController` models Unreal's character movement: acceleration
and braking on the ground, minimal air control, and a jump impulse. Tune it
with `SetMaxWalkSpeed`, `SetSprintSpeedMultiplier`, `SetJumpZVelocity`,
`SetMaxAcceleration`, `SetBrakingDeceleration`, `SetGroundFriction`, and
`SetAirControl`. `SetFlying(true)` swaps to noclip-style free flight, which
is useful while you are still building the level.

For a debug or spectator camera with no physics at all, use
`Sleak::FreeLookCameraController` instead, as in
@ref getting_started "Getting Started".

## Give the world something to collide with

Anything with a collider joins the broadphase. Static geometry gets an AABB
and a `Static` body:

```cpp
auto* floor = Sleak::GameObject::CreateCube(
    Sleak::Math::Vector3D(0.0f, -0.5f, 0.0f));
floor->GetComponent<Sleak::TransformComponent>()->SetScale(
    Sleak::Math::Vector3D(40.0f, 1.0f, 40.0f));
floor->AddComponent<Sleak::ColliderComponent>(
    Sleak::Physics::AABB(Sleak::Math::Vector3D(-0.5f, -0.5f, -0.5f),
                         Sleak::Math::Vector3D(0.5f, 0.5f, 0.5f)));
floor->AddComponent<Sleak::RigidbodyComponent>(Sleak::BodyType::Static);
AddObject(floor);
```

`ColliderComponent` also constructs from a `BoundingSphere`, a
`BoundingCapsule`, or straight from `MeshData` when you want the shape
derived from the geometry.

## Query the world

`Sleak::Physics::PhysicsWorld` answers questions about the scene without
you tracking objects yourself. Get it from the scene, then cast:

```cpp
#include <Physics/PhysicsWorld.hpp>

if (auto* world = GetPhysicsWorld()) {
    Sleak::Physics::RayHit hit = world->Raycast(
        cam->GetPosition(), cam->GetDirection(), /*maxDist=*/5.0f);

    if (hit.hit) {
        SLEAK_INFO("Looking at {} at distance {}",
                   hit.collider->GetOwner()->GetName(), hit.distance);
    }
}
```

`OverlapSphere`, `OverlapAABB`, and `SphereSweep` round out the query set.
Every one of them takes an optional `layerMask`, so pair it with
`ColliderComponent::SetLayer` to keep, say, a bullet raycast from hitting
trigger volumes.

---

## Handle input

Input arrives as events. Register a member function once, and the
dispatcher calls it for every event of that type.

```cpp
// FirstScene.hpp
#include <Events/KeyboardEvent.hpp>

class FirstScene : public Sleak::Scene {
public:
    // ...
    void OnKeyPressed(const Sleak::Events::Input::KeyPressedEvent& e);

private:
    std::string m_keyHandlerId;
};
```

```cpp
// FirstScene.cpp
void FirstScene::Begin() {
    // ... build the scene ...
    m_keyHandlerId = Sleak::EventDispatcher::RegisterEventHandler(
        this, &FirstScene::OnKeyPressed);

    Sleak::Scene::Begin();
}

void FirstScene::OnKeyPressed(
    const Sleak::Events::Input::KeyPressedEvent& e) {
    if_key_press(KEY__F) {
        if (auto* cam = GetActiveCamera()) {
            if (auto* ctrl =
                    cam->GetComponent<Sleak::FreeLookCameraController>()) {
                ctrl->SetEnabled(!ctrl->IsEnabled());
            }
        }
    }
}
```

`if_key_press(KEY__F)` expands to a check on the local variable named `e`,
so name your event parameter `e`. It ignores OS auto-repeat, which is what
you want for a toggle. Use `if_key_down` when you want the repeat, and read
`e.GetKeyCode()` directly for anything more involved.

**Unregister what you register.** Handlers outlive the scene otherwise, and
the next event dispatched calls into freed memory. Do it in `OnDeactivate`
or the destructor:

```cpp
void FirstScene::OnDeactivate() {
    if (!m_keyHandlerId.empty()) {
        Sleak::EventDispatcher::UnregisterEvent(
            Sleak::EventType::KeyPressed, m_keyHandlerId);
        m_keyHandlerId.clear();
    }
    Sleak::Scene::OnDeactivate();
}
```

The event types you will use most are
`Sleak::Events::Input::KeyPressedEvent`, `KeyReleasedEvent`,
`MouseMovedEvent`, `MouseButtonPressedEvent`, and
`Sleak::Events::WindowResizeEvent`. See
@ref events_and_input "Events and Input" for the full hierarchy.

---

## Draw a HUD

`Sleak::UI` is available from any scene update. It is immediate mode: you
call it every frame, and there is no widget tree to maintain.

```cpp
#include <Core/Application.hpp>
#include <UI/UI.hpp>

void FirstScene::Update(float deltaTime) {
    Sleak::Scene::Update(deltaTime);

    namespace UI = Sleak::UI;
    auto* app = Sleak::Application::GetInstance();

    UI::BeginPanel("Stats", 10.0f, 10.0f, 0.6f,
                   UI::PanelFlags_AutoResize | UI::PanelFlags_NoMove);

    UI::Text("Backend: %s", app->GetRendererTypeStr());
    UI::Text("FPS: %d", app->GetFPS());
    UI::Text("Frame: %.2f ms", app->GetFrameTime() * 1000.0f);
    UI::Separator();
    UI::Text("Triangles: %d", app->GetTriangles());

    bool vsync = app->GetVSync();
    if (UI::Checkbox("VSync", &vsync)) app->SetVSync(vsync);

    UI::EndPanel();
}
```

Widgets return `true` on the frame they change, which is why the pattern is
always `if (UI::Checkbox(...)) apply(...)`. Beyond text and buttons you get
`DragFloat`, `Combo`, `InputText`, `Selectable`, `ProgressBar`, list boxes,
child regions, style push and pop, and direct 2D drawing through
`DrawLine`, `DrawRect`, `DrawFilledRect`, and `DrawImage`.

Never include `imgui.h` from your game. Dear ImGui is a private dependency
of the engine target; if `Sleak::UI` is missing something you need, that
wrapper is where it should be added.

---

## Add a second scene and switch to it

Register every scene up front, then switch whenever you like:

```cpp
bool Game::Initialize() {
    auto* menu = new MenuScene();
    auto* world = new FirstScene();
    AddScene(menu);
    AddScene(world);
    SetActiveScene(menu);
    return true;
}
```

When you switch scenes at runtime, and especially when you tear one down,
flush the GPU first. Destroying resources the GPU is still reading from
crashes the driver.

```cpp
void Game::StartWorld() {
    auto* app = Sleak::Application::GetInstance();
    if (app) app->WaitGPUIdle();

    if (m_worldScene) RemoveScene(m_worldScene);

    m_worldScene = new FirstScene();
    AddScene(m_worldScene);
    SetActiveScene(m_worldScene);
}
```

`RemoveScene` unloads and deletes the scene, and unloading destroys every
object it owns.

---

## Tune the renderer

Per-frame quality settings live on the `Application` and can be changed at
any time, which makes them easy to wire straight into a settings panel:

```cpp
auto* app = Sleak::Application::GetInstance();
app->SetMSAASampleCount(8);
app->SetVSync(true);
app->SetSSAOEnabled(true);
app->SetSSAORadius(0.5f);
app->SetBloomEnabled(true);
app->SetTAAEnabled(true);
```

Post-processing settings apply to the deferred path, which not every
backend implements. Check first rather than assuming:

```cpp
uint32_t caps = app->GetGraphicsCaps();
```

To ship a whole quality preset in one call, fill a
`Sleak::GraphicsConfig` and pass it to `Application::ApplyGraphicsConfig`.

---

## Where to go next

- @ref scene_and_objects "Scenes, Objects, and Components" for the
  ownership and lifecycle rules in full.
- @ref physics "Physics and Spatial Partitioning" for the broadphase tree
  and collision resolution.
- @ref culling "Culling" for submitting occluders and reading back cull
  statistics.
- @ref vertex_format "Vertex Format Registration" when the built-in vertex
  layout does not describe your geometry.
- @ref rendering_pipeline "Rendering Pipeline" for what happens between
  your draw submission and the swapchain.
