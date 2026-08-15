# Tutorial: A Small Game {#tutorial_small_game}

@ref your_first_scene "Your First Scene" showed the pieces in isolation.
This tutorial wires them into a loop you can lose and win: walk a
first-person body around a floor, collect five cubes, watch a counter on
the HUD, and switch to a results scene when the last one is gone.

Everything here builds on the project layout from
@ref getting_started "Getting Started": a `Game` class deriving from
`Sleak::GameBase`, scenes deriving from `Sleak::Scene`, and a `Client`
executable that copies assets next to the binary.

---

## What you are building

| Piece | Engine type |
|---|---|
| Ground and pickups | `Sleak::GameObject` primitives plus `Sleak::ColliderComponent` |
| Player body | `Sleak::Camera` with `Sleak::FirstPersonController` and a rigidbody |
| Lighting | `Sleak::DirectionalLight` and the scene's `Sleak::LightManager` |
| Input | `Sleak::EventDispatcher` handlers for key press and release |
| Pickup detection | `Sleak::Physics::PhysicsWorld::OverlapSphere` |
| HUD | `Sleak::UI` immediate-mode calls from `Update` |
| Screen change | `Sleak::GameBase` scene swap after `Application::WaitGPUIdle` |

---

## 1. Declare the scene

The scene holds three kinds of state: the objects it needs to find again
later, the handler ids it must unregister, and the gameplay counters.

```cpp
// Game/include/Scenes/PlayScene.hpp
#ifndef _PLAY_SCENE_HPP_
#define _PLAY_SCENE_HPP_

#include <Core/Scene.hpp>
#include <Events/KeyboardEvent.hpp>

#include <string>
#include <vector>

namespace Sleak {
class Camera;
class GameObject;
}

class PlayScene : public Sleak::Scene {
public:
    PlayScene() : Sleak::Scene("PlayScene") {}
    ~PlayScene() override;

    void Begin() override;
    void Update(float deltaTime) override;
    void OnDeactivate() override;

    void OnKeyPressed(const Sleak::Events::Input::KeyPressedEvent& e);
    void OnKeyReleased(const Sleak::Events::Input::KeyReleasedEvent& e);

private:
    void SpawnPickups();
    void CollectNearbyPickups();
    void DrawHud();
    void FinishRun();

    Sleak::Camera* m_player = nullptr;
    Sleak::GameObject* m_platform = nullptr;
    std::vector<Sleak::GameObject*> m_pickups;

    std::string m_keyDownId;
    std::string m_keyUpId;

    bool m_platformLeft = false;
    bool m_platformRight = false;

    int m_collected = 0;
    int m_total = 0;
    float m_elapsed = 0.0f;
};

#endif  // _PLAY_SCENE_HPP_
```

Two layer bits keep the pickup query from picking up the floor:

```cpp
// Game/include/Scenes/PlayScene.hpp, above the class
constexpr uint32_t LAYER_WORLD  = 1u << 0;
constexpr uint32_t LAYER_PICKUP = 1u << 1;
```

`ColliderComponent` starts at layer `0xFFFFFFFF`, which matches every mask.
Assign explicit bits to anything you intend to query for, or every query
returns everything.

## 2. Build the ground

Static level geometry is a primitive with an `AABB` collider and a `Static`
rigidbody. The collider shape is in local space, so scale the transform and
size the shape to match.

```cpp
// Game/src/Scenes/PlayScene.cpp
#include "Scenes/PlayScene.hpp"

#include <Camera/Camera.hpp>
#include <Core/Application.hpp>
#include <Core/GameObject.hpp>
#include <Core/Logger.hpp>
#include <ECS/Components/FirstPersonController.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Lighting/DirectionalLight.hpp>
#include <Lighting/LightManager.hpp>
#include <Physics/ColliderComponent.hpp>
#include <Physics/PhysicsWorld.hpp>
#include <Physics/RigidbodyComponent.hpp>
#include <UI/UI.hpp>

void PlayScene::Begin() {
    // --- Ground ---
    auto* floor = Sleak::GameObject::CreateCube(
        Sleak::Math::Vector3D(0.0f, -0.5f, 0.0f));
    floor->SetTag("Floor");
    floor->GetComponent<Sleak::TransformComponent>()->SetScale(
        Sleak::Math::Vector3D(60.0f, 1.0f, 60.0f));
    floor->AddComponent<Sleak::ColliderComponent>(
        Sleak::Physics::AABB(Sleak::Math::Vector3D(-30.0f, -0.5f, -30.0f),
                             Sleak::Math::Vector3D(30.0f, 0.5f, 30.0f)));
    floor->AddComponent<Sleak::RigidbodyComponent>(Sleak::BodyType::Static);
    if (auto* c = floor->GetComponent<Sleak::ColliderComponent>())
        c->SetLayer(LAYER_WORLD);
    AddObject(floor);
```

## 3. Give the player a body

The camera carries the controller, the collider, and the rigidbody. Attach
the components first, then `AddObject`, because that call is what registers
the collider with the scene's `Sleak::Physics::PhysicsWorld`.

```cpp
    // --- Player ---
    m_player = new Sleak::Camera("Player",
                                 Sleak::Math::Vector3D(0.0f, 2.0f, -8.0f),
                                 70.0f, 0.1f, 500.0f);
    m_player->SetDirection(Sleak::Math::Vector3D(0.0f, 0.0f, 1.0f));

    m_player->AddComponent<Sleak::FirstPersonController>();
    m_player->AddComponent<Sleak::ColliderComponent>(
        Sleak::Physics::BoundingSphere(Sleak::Math::Vector3D(0, 0, 0), 0.4f));
    m_player->AddComponent<Sleak::RigidbodyComponent>(
        Sleak::BodyType::Dynamic);

    if (auto* c = m_player->GetComponent<Sleak::ColliderComponent>())
        c->SetLayer(LAYER_WORLD);

    if (auto* rb = m_player->GetComponent<Sleak::RigidbodyComponent>()) {
        rb->SetUseGravity(true);
        rb->SetGravity(Sleak::Math::Vector3D(0.0f, -24.0f, 0.0f));
    }

    if (auto* fpc = m_player->GetComponent<Sleak::FirstPersonController>()) {
        fpc->SetMaxWalkSpeed(6.0f);
        fpc->SetJumpZVelocity(9.0f);
    }

    AddObject(m_player);
    m_player->Initialize();
    SetActiveCamera(m_player);
```

## 4. Light it

One key light with shadows and a small ambient term is enough to read the
scene. Lights are `GameObject`s, so they go in through `AddObject` and the
scene registers them with the `Sleak::LightManager` for you.

```cpp
    // --- Sun ---
    auto* sun = new Sleak::DirectionalLight("Sun");
    sun->SetDirection(Sleak::Math::Vector3D(-0.4f, -0.85f, -0.3f));
    sun->SetColor(1.0f, 0.96f, 0.88f);
    sun->SetIntensity(4.0f);
    sun->SetCastShadows(true);
    AddObject(sun);

    if (auto* lm = GetLightManager()) {
        lm->SetAmbientColor(0.10f, 0.12f, 0.16f);
        lm->SetAmbientIntensity(0.55f);
    }
```

## 5. Spawn the pickups

Pickups are triggers: they belong to the broadphase so queries find them,
but they generate no physics response, so the player walks straight through
one instead of bouncing off it.

```cpp
    SpawnPickups();

    // --- Movable platform, driven by input in step 6 ---
    m_platform = Sleak::GameObject::CreateCube(
        Sleak::Math::Vector3D(0.0f, 1.0f, 6.0f));
    m_platform->SetTag("Platform");
    m_platform->GetComponent<Sleak::TransformComponent>()->SetScale(
        Sleak::Math::Vector3D(4.0f, 0.5f, 4.0f));
    m_platform->AddComponent<Sleak::ColliderComponent>(
        Sleak::Physics::AABB(Sleak::Math::Vector3D(-2.0f, -0.25f, -2.0f),
                             Sleak::Math::Vector3D(2.0f, 0.25f, 2.0f)));
    m_platform->AddComponent<Sleak::RigidbodyComponent>(
        Sleak::BodyType::Kinematic);
    if (auto* c = m_platform->GetComponent<Sleak::ColliderComponent>())
        c->SetLayer(LAYER_WORLD);
    AddObject(m_platform);

    // --- Input ---
    m_keyDownId = Sleak::EventDispatcher::RegisterEventHandler(
        this, &PlayScene::OnKeyPressed);
    m_keyUpId = Sleak::EventDispatcher::RegisterEventHandler(
        this, &PlayScene::OnKeyReleased);

    Sleak::Scene::Begin();
}

void PlayScene::SpawnPickups() {
    const Sleak::Math::Vector3D spots[] = {
        Sleak::Math::Vector3D(-8.0f, 1.0f, 4.0f),
        Sleak::Math::Vector3D(6.0f, 1.0f, 10.0f),
        Sleak::Math::Vector3D(-3.0f, 1.0f, 16.0f),
        Sleak::Math::Vector3D(11.0f, 1.0f, -5.0f),
        Sleak::Math::Vector3D(0.0f, 1.0f, -14.0f),
    };

    for (const auto& spot : spots) {
        auto* pickup = Sleak::GameObject::CreateCube(spot);
        pickup->SetTag("Pickup");
        pickup->GetComponent<Sleak::TransformComponent>()->SetScale(
            Sleak::Math::Vector3D(0.5f, 0.5f, 0.5f));
        pickup->AddComponent<Sleak::ColliderComponent>(
            Sleak::Physics::AABB(
                Sleak::Math::Vector3D(-0.25f, -0.25f, -0.25f),
                Sleak::Math::Vector3D(0.25f, 0.25f, 0.25f)));

        if (auto* c = pickup->GetComponent<Sleak::ColliderComponent>()) {
            c->SetTrigger(true);
            c->SetLayer(LAYER_PICKUP);
        }

        AddObject(pickup);
        m_pickups.push_back(pickup);
    }

    m_total = static_cast<int>(m_pickups.size());
}
```

`Sleak::GameObject::CreateCube`, `CreatePlane`, `CreateSphere`,
`CreateCapsule`, `CreateCylinder`, and `CreateTorus` build the mesh and
transform for you, which is what you want while the level is still made of
boxes. Swap in `Sleak::ModelLoader::Load` when real art arrives.

## 6. Move something with input

Key state arrives as events, not as a queryable snapshot, so a held key is
something you track yourself: set a flag on `KeyPressedEvent`, clear it on
`KeyReleasedEvent`, and read the flag during `Update`.

```cpp
void PlayScene::OnKeyPressed(
    const Sleak::Events::Input::KeyPressedEvent& e) {
    if_key_down(KEY__LEFT) m_platformLeft = true;
    if_key_down(KEY__RIGHT) m_platformRight = true;

    if_key_press(KEY__R) {
        SLEAK_INFO("Run reset after {:.1f}s", m_elapsed);
        m_collected = 0;
        m_elapsed = 0.0f;
    }
}

void PlayScene::OnKeyReleased(
    const Sleak::Events::Input::KeyReleasedEvent& e) {
    if_key_down(KEY__LEFT) m_platformLeft = false;
    if_key_down(KEY__RIGHT) m_platformRight = false;
}
```

`if_key_down(KEY__LEFT)` expands to a test on a local named `e`, so keep
that parameter name. Use `if_key_press` for edge-triggered actions: it
additionally rejects OS auto-repeat, which is what stops a held `R` from
resetting the run sixty times a second.

Apply the flags where the rest of your per-frame work happens:

```cpp
void PlayScene::Update(float deltaTime) {
    Sleak::Scene::Update(deltaTime);
    m_elapsed += deltaTime;

    if (m_platform) {
        float dir = (m_platformRight ? 1.0f : 0.0f) -
                    (m_platformLeft ? 1.0f : 0.0f);
        if (dir != 0.0f) {
            m_platform->GetComponent<Sleak::TransformComponent>()->Translate(
                Sleak::Math::Vector3D(dir * 5.0f * deltaTime, 0.0f, 0.0f));
        }
    }

    CollectNearbyPickups();
    DrawHud();
}
```

A `Kinematic` body moves under your control and pushes dynamic bodies
without being pushed back, which is exactly the behavior a platform wants.

## 7. React to a collision

The engine resolves collisions inside `PhysicsWorld::Step`; it does not
call back into your scene when two shapes touch. Gameplay reactions are
queries you run yourself, and the query API is fast enough to run every
frame because it goes through the broadphase tree.

```cpp
void PlayScene::CollectNearbyPickups() {
    auto* world = GetPhysicsWorld();
    if (!world || !m_player) return;

    const float reach = 1.4f;
    const Sleak::Math::Vector3D playerPos = m_player->GetPosition();

    for (const auto& pair :
         world->OverlapSphere(playerPos, reach, LAYER_PICKUP)) {
        if (!pair.b) continue;

        Sleak::GameObject* obj = pair.b->GetOwner();
        if (!obj || obj->IsPendingDestroy()) continue;

        // OverlapSphere reports broadphase AABB overlaps, so confirm the
        // real distance before treating it as a touch.
        Sleak::Math::Vector3D toPickup =
            obj->GetComponent<Sleak::TransformComponent>()
                ->GetWorldPosition() -
            playerPos;
        if (toPickup.Magnitude() > reach) continue;

        DestroyObject(obj);
        ++m_collected;
        SLEAK_INFO("Collected {} ({}/{})", obj->GetName(), m_collected,
                   m_total);
    }

    if (m_collected >= m_total) FinishRun();
}
```

`CollisionPair::b` is the collider the query found; `a` stays null for
overlap queries, since there is no second collider involved. `DestroyObject`
queues the object and the scene frees it at a safe point later in the
frame, which is why it is safe to call while iterating query results.
`Raycast`, `SphereSweep`, and `OverlapAABB` take the same optional
`layerMask` and are the right tools for interaction prompts, footstep
checks, and blast radii.

## 8. Draw the HUD

`Sleak::UI` is immediate mode. Call it every frame from your update; there
is no widget tree to keep in sync.

```cpp
void PlayScene::DrawHud() {
    namespace UI = Sleak::UI;
    auto* app = Sleak::Application::GetInstance();

    UI::BeginPanel("Run", 16.0f, 16.0f, 0.55f,
                   UI::PanelFlags_AutoResize | UI::PanelFlags_NoMove |
                       UI::PanelFlags_NoTitleBar);

    UI::Text("Collected  %d / %d", m_collected, m_total);
    UI::Text("Time       %.1fs", m_elapsed);
    UI::Separator();
    UI::Text("Arrows move the platform, R restarts");
    if (app) UI::TextDisabled("%s  %d FPS", app->GetRendererTypeStr(),
                              app->GetFPS());

    UI::EndPanel();
}
```

Widgets return `true` on the frame they change, so the standard shape is
`if (UI::Button("Restart")) Restart();`. Never include `imgui.h` from game
code: Dear ImGui is a private dependency of the engine target, and
`UI/UI.hpp` is where a missing wrapper belongs.

## 9. Switch scenes safely

Ending the run means handing control back to the game object, because the
scene registry lives on `Sleak::GameBase`, not on any single scene.

```cpp
// Game/include/Game.hpp
class SLEAK_API Game : public Sleak::GameBase {
public:
    bool Initialize() override;
    void Begin() override {}
    void Loop(float deltaTime) override {}
    bool GetIsGameRunning() override { return bIsGameRunning; }

    void ShowResults(int collected, float seconds);

private:
    class PlayScene* m_play = nullptr;
    class ResultsScene* m_results = nullptr;
    bool bIsGameRunning = true;
};
```

```cpp
// Game/src/Game.cpp
bool Game::Initialize() {
    m_play = new PlayScene();
    m_results = new ResultsScene();
    AddScene(m_play);
    AddScene(m_results);
    SetActiveScene(m_play);
    return true;
}

void Game::ShowResults(int collected, float seconds) {
    if (auto* app = Sleak::Application::GetInstance()) app->WaitGPUIdle();

    m_results->SetSummary(collected, seconds);
    SetActiveScene(m_results);
}
```

`WaitGPUIdle` before the swap is not optional. The GPU is still reading
buffers and images owned by scene objects, and freeing them underneath an
in-flight frame crashes the driver. The same rule applies to `RemoveScene`,
which unloads and deletes the scene along with every object it owns.

The scene side just asks the game to do it:

```cpp
void PlayScene::FinishRun() {
    auto* app = Sleak::Application::GetInstance();
    if (!app) return;
    if (auto* game = static_cast<Game*>(app->GetGame()))
        game->ShowResults(m_collected, m_elapsed);
}
```

## 10. Unregister what you registered

Handlers outlive the scene unless you remove them, and the next dispatched
event then calls into freed memory. Unregister in `OnDeactivate`, the
destructor, or both.

```cpp
void PlayScene::OnDeactivate() {
    if (!m_keyDownId.empty()) {
        Sleak::EventDispatcher::UnregisterEvent(
            Sleak::EventType::KeyPressed, m_keyDownId);
        m_keyDownId.clear();
    }
    if (!m_keyUpId.empty()) {
        Sleak::EventDispatcher::UnregisterEvent(
            Sleak::EventType::KeyReleased, m_keyUpId);
        m_keyUpId.clear();
    }
    Sleak::Scene::OnDeactivate();
}

PlayScene::~PlayScene() { PlayScene::OnDeactivate(); }
```

Objects need no such care: the scene owns everything passed to `AddObject`
and destroys all of it on unload.

---

## Where to go next

- @ref physics "Physics and Spatial Partitioning" for the broadphase tree,
  collider shapes, and the full query set.
- @ref events_and_input "Events and Input" for the event hierarchy behind
  the two handlers above.
- @ref scene_and_objects "Scenes, Objects, and Components" for the
  ownership and lifecycle rules in detail.
- @ref performance_guide "Performance Guide" once the scene has more in it
  than five cubes.
- @ref building_shipping "Building and Shipping" when it is time to hand
  the build to someone else.
