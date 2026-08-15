# Getting Started {#getting_started}

This guide takes you from an empty directory to a running window with a lit
cube, a skybox, and a camera you can fly around. Every snippet is taken
from a project that builds against the current engine.

By the end you will have the four pieces every SleakEngine game needs:

1. A **CMake project** that pulls the engine in as a subdirectory.
2. An **entry point** that creates a `Sleak::Application` and runs it.
3. A **game class** deriving from `Sleak::GameBase` that owns your scenes.
4. A **scene class** deriving from `Sleak::Scene` that owns your objects.

---

## 1. Lay out the project

The engine expects to sit alongside your code, not inside it. A working
layout looks like this:

```
MyGame/
├── CMakeLists.txt
├── Engine/               # SleakEngine, cloned or symlinked here
├── Game/
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── Game.hpp
│   │   └── Scenes/FirstScene.hpp
│   ├── src/
│   │   ├── Game.cpp
│   │   └── Scenes/FirstScene.cpp
│   └── assets/           # your textures, models, shaders, skybox
└── Client/
    ├── CMakeLists.txt
    └── src/main.cpp
```

`Game` builds as a shared library and holds all your gameplay code.
`Client` is the executable, and it exists mostly to hold `main()` and to
copy assets next to the binary.

---

## 2. Wire up CMake

**Root `CMakeLists.txt`.** Send all build artifacts to one `bin/`
directory, then add the three subprojects in dependency order.

```cmake
cmake_minimum_required(VERSION 3.31)
project(MyGame LANGUAGES CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} /utf-8")
endif()

add_subdirectory(Engine)
add_subdirectory(Game)
add_subdirectory(Client)
```

**`Game/CMakeLists.txt`.** The one thing to get right is the include path:
your game must see `Engine/include/public` and nothing else from the
engine tree.

```cmake
cmake_minimum_required(VERSION 3.31)
project(MyGame_Game)

file(GLOB_RECURSE GAME_SOURCES "src/*.cpp")

add_library(SleakGame SHARED ${GAME_SOURCES})

target_compile_definitions(SleakGame PRIVATE SLEAK_EXPORTS)

target_include_directories(SleakGame PUBLIC
                           ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_SOURCE_DIR}/Engine/include/public)

target_link_libraries(SleakGame PRIVATE Engine)
```

`SLEAK_EXPORTS` switches the `SLEAK_API` macro from import to export, so
mark your public game classes with it: `class SLEAK_API Game : public
Sleak::GameBase`. The engine's own exported symbols use `ENGINE_API`; both
macros come from `Core/OSDef.hpp`.

**`Client/CMakeLists.txt`.** Link the game and the engine, then copy both
asset trees next to the executable after every build.

```cmake
cmake_minimum_required(VERSION 3.31)
project(MyGame_Client)

file(GLOB_RECURSE CLIENT_SOURCES "src/*.cpp")

add_executable(MyGame ${CLIENT_SOURCES})

target_link_libraries(MyGame PRIVATE SleakGame Engine)
target_include_directories(MyGame PRIVATE ${CMAKE_SOURCE_DIR}/Game/include)

add_custom_command(TARGET MyGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Engine/assets
        $<TARGET_FILE_DIR:MyGame>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Game/assets
        $<TARGET_FILE_DIR:MyGame>/assets
    COMMENT "Copying assets to output directory")

if(WIN32)
    add_custom_command(TARGET MyGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:MyGame>
            $<TARGET_FILE_DIR:MyGame>
        COMMAND_EXPAND_LISTS)
endif()
```

That copy step matters more than it looks. Every asset path you write in
code is resolved relative to the executable's directory, so
`"assets/skybox/right.jpg"` means `bin/assets/skybox/right.jpg`. Edit an
asset and you have to rebuild for the copy to run again.

---

## 3. Write the entry point

`main()` does four things in order: point the working directory at the
executable, parse the command line, initialize logging, and run the
application.

```cpp
// Client/src/main.cpp
#include <Core/Application.hpp>
#include <Core/CommandLine.hpp>
#include <Core/Logger.hpp>
#include <Game.hpp>

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

#define PROJECT_NAME "MyGame"

static void SetWorkingDirToExePath(char* argv0) {
#ifdef _WIN32
    char path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) != 0) {
        std::filesystem::current_path(
            std::filesystem::path(path).parent_path());
        return;
    }
#endif
    auto exeDir = std::filesystem::path(argv0).parent_path();
    if (!exeDir.empty()) std::filesystem::current_path(exeDir);
}

int main(int argc, char** argv) {
    SetWorkingDirToExePath(argv[0]);

    Sleak::CommandLine::Parse(argc, argv);
    Sleak::Logger::Init(PROJECT_NAME);

    Sleak::ApplicationDefaults defaults{
        .Name = PROJECT_NAME,
        .CommandLineArgs = Sleak::Arguments(argc, argv)};

    Game* game = new Game();
    Sleak::Application app(defaults);

    return app.Run(game);
}
```

Call `Sleak::CommandLine::Parse` **before** constructing the
`Application`. The application reads its window size and backend choice out
of the parsed table, so skipping this step silently disables every flag.

Once it is in place you get these for free:

| Flag | Effect |
|---|---|
| `-r vulkan\|opengl\|d3d11\|d3d12` | Choose the graphics backend |
| `-w <n>` / `-h <n>` | Window width and height |
| `-t My_Window_Title` | Window title, with `_` standing in for spaces |
| `--fullscreen` | Start fullscreen |

Add your own flags anywhere in your game with
`Sleak::CommandLine::GetValue("-seed")` and
`Sleak::CommandLine::HasFlag("--fly")`. To print usage for `--help`,
register a callback with `Sleak::CommandLine::SetHelpCallback` before
`Parse`.

`Sleak::Logger::Init` must run before any logging macro. After it, use
`SLEAK_LOG`, `SLEAK_INFO`, `SLEAK_WARN`, and `SLEAK_ERROR` with fmt-style
`{}` placeholders from anywhere.

---

## 4. Write the game class

`Sleak::GameBase` owns your scene registry. Three methods are pure virtual,
so you implement all three even if two stay empty at first.

```cpp
// Game/include/Game.hpp
#ifndef _GAME_HPP_
#define _GAME_HPP_

#include <Core/GameBase.hpp>
#include <Core/OSDef.hpp>

class SLEAK_API Game : public Sleak::GameBase {
public:
    Game() = default;
    ~Game() override = default;

    bool Initialize() override;
    void Begin() override;
    void Loop(float deltaTime) override;

    bool GetIsGameRunning() override { return bIsGameRunning; }

private:
    bool bIsGameRunning = true;
};

#endif  // _GAME_HPP_
```

```cpp
// Game/src/Game.cpp
#include "Game.hpp"

#include <Core/Application.hpp>

#include "Scenes/FirstScene.hpp"

bool Game::Initialize() {
    Sleak::Application::GetInstance()->SetMSAASampleCount(8);
    Sleak::Application::GetInstance()->SetVSync(true);

    auto* first = new FirstScene();
    AddScene(first);
    SetActiveScene(first);

    return true;
}

void Game::Begin() {}

void Game::Loop(float deltaTime) {}
```

`AddScene` hands ownership to `GameBase`, which unloads and deletes every
registered scene in its destructor. `SetActiveScene` deactivates whatever
was active and activates the new one, which triggers the scene's load and
`Begin`. Returning `false` from `Initialize` aborts the run before the loop
starts.

`Loop` runs once per frame, after the active scene has updated. Anything
global to your game and independent of the current scene goes here.

---

## 5. Write the scene

`Sleak::Scene` gives you the full lifecycle. Override only what you need;
each override should call its base implementation.

```cpp
// Game/include/Scenes/FirstScene.hpp
#ifndef _FIRST_SCENE_HPP_
#define _FIRST_SCENE_HPP_

#include <Core/Scene.hpp>

class FirstScene : public Sleak::Scene {
public:
    FirstScene() : Sleak::Scene("FirstScene") {}
    ~FirstScene() override = default;

    void Begin() override;
    void Update(float deltaTime) override;
};

#endif  // _FIRST_SCENE_HPP_
```

The lifecycle hooks, in the order they fire:

| Hook | When it runs |
|---|---|
| `OnLoad()` | Once, when the scene is first loaded. Load assets here. |
| `Initialize()` | Once, after `OnLoad`. |
| `Begin()` | Once, after `Initialize`. Build your object graph here. |
| `OnActivate()` | Every time the scene becomes the active scene. |
| `Update(dt)` | Every frame while active. |
| `FixedUpdate(dt)` | On the fixed timestep, unless you call `SetFixedUpdateEnabled(false)`. |
| `LateUpdate(dt)` | Every frame, after all `Update` calls. |
| `OnDeactivate()` | When another scene takes over. |
| `OnUnload()` | Once, on teardown. Release what `OnLoad` acquired. |

Now populate it. This is the whole scene: a material, a cube, a skybox,
three lights, and a camera.

```cpp
// Game/src/Scenes/FirstScene.cpp
#include "Scenes/FirstScene.hpp"

#include <Camera/Camera.hpp>
#include <Core/GameObject.hpp>
#include <ECS/Components/FreeLookCameraController.hpp>
#include <ECS/Components/MaterialComponent.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Lighting/DirectionalLight.hpp>
#include <Lighting/LightManager.hpp>
#include <Math/Vector.hpp>
#include <Runtime/Material.hpp>
#include <Runtime/Skybox.hpp>

#include <cmath>

void FirstScene::Begin() {
    // --- Material ---
    auto* mat = new Sleak::Material();
    mat->SetShader("assets/shaders/default_shader.hlsl");
    mat->SetDiffuseColor((uint8_t)230, (uint8_t)230, (uint8_t)230);
    mat->SetMetallic(0.0f);
    mat->SetRoughness(0.35f);
    mat->SetAO(1.0f);
    mat->SetOpacity(1.0f);

    // --- Cube ---
    auto* cube = Sleak::GameObject::CreateCube(
        Sleak::Math::Vector3D(0.0f, 0.0f, 0.0f));
    cube->SetTag("Cube");
    cube->AddComponent<Sleak::MaterialComponent>(
        Sleak::RefPtr<Sleak::Material>(mat));
    AddObject(cube);

    // --- Skybox: +X, -X, +Y, -Y, +Z, -Z ---
    SetSkybox(new Sleak::Skybox(std::array<std::string, 6>{
        "assets/skybox/right.jpg", "assets/skybox/left.jpg",
        "assets/skybox/top.jpg", "assets/skybox/bottom.jpg",
        "assets/skybox/front.jpg", "assets/skybox/back.jpg"}));

    // --- Key light ---
    auto* sun = new Sleak::DirectionalLight("Sun");
    sun->SetDirection(Sleak::Math::Vector3D(-0.5f, -0.75f, -0.3f));
    sun->SetColor(1.0f, 0.95f, 0.85f);
    sun->SetIntensity(4.0f);
    sun->SetCastShadows(true);
    AddObject(sun);

    // --- Fill light, standing in for sky bounce ---
    auto* fill = new Sleak::DirectionalLight("FillLight");
    fill->SetDirection(Sleak::Math::Vector3D(0.5f, -0.3f, 0.4f));
    fill->SetColor(0.6f, 0.75f, 1.0f);
    fill->SetIntensity(0.8f);
    fill->SetCastShadows(false);
    AddObject(fill);

    // --- Ambient and fog ---
    if (auto* lm = GetLightManager()) {
        lm->SetAmbientColor(0.08f, 0.10f, 0.15f);
        lm->SetAmbientIntensity(0.6f);
        lm->SetFogColor(0.55f, 0.62f, 0.78f);
        lm->SetFogDistances(40.0f, 100.0f);
        lm->SetFogEnabled(true);
    }

    // --- Camera ---
    Sleak::Math::Vector3D camPos(-5.0f, 3.0f, -5.0f);
    Sleak::Math::Vector3D target(0.0f, 0.0f, 0.0f);
    Sleak::Math::Vector3D forward = (target - camPos).Normalized();

    auto* camera =
        new Sleak::Camera("MainCamera", camPos, 60.0f, 0.01f, 200.0f);
    camera->SetLookTarget(target);
    camera->AddComponent<Sleak::FreeLookCameraController>();
    camera->Initialize();

    if (auto* ctrl =
            camera->GetComponent<Sleak::FreeLookCameraController>()) {
        ctrl->SetEnabled(true);
        ctrl->SetYaw(std::atan2(forward.GetX(), forward.GetZ()));
        ctrl->SetPitch(-std::asin(forward.GetY()));
    }

    AddObject(camera);
    SetActiveCamera(camera);

    Sleak::Scene::Begin();
}

void FirstScene::Update(float deltaTime) {
    Sleak::Scene::Update(deltaTime);
}
```

A few things worth pulling out of that block.

**Objects belong to the scene.** `AddObject` transfers ownership. Never
`delete` a `GameObject` yourself: call `RemoveObject` to destroy it now, or
`DestroyObject` to queue it for the end of the frame. `Sleak::Scene::Begin`
is called last on purpose, since it activates everything you just added.

**Lights are GameObjects too.** `Sleak::DirectionalLight` derives from
`Sleak::GameObject`, so it goes in through `AddObject` like anything else,
and the scene registers it with the `Sleak::LightManager` for you.

**`CreateCube` and friends are prototyping helpers.**
`Sleak::GameObject::CreatePlane`, `CreateCube`, `CreateSphere`,
`CreateCapsule`, `CreateCylinder`, and `CreateTorus` build a mesh and
transform for you. Real content comes in through
`Sleak::ModelLoader::Load`, covered in the next guide.

**The camera controller needs `Initialize()` called explicitly** before you
query it, because it derives yaw and pitch from the camera's current
facing. Syncing yaw and pitch afterward keeps the first mouse movement from
snapping the view.

---

## 6. Build and run

```bash
cmake -S . -B build
cmake --build build
./bin/MyGame
```

Drive the free-look camera with `W`/`A`/`S`/`D`, hold `LCTRL` to move
faster, and steer with the mouse. `F11` toggles fullscreen and `Esc`
closes the window.

Try a different backend to confirm the abstraction holds:

```bash
./bin/MyGame -r opengl -w 1600 -h 900
```

---

## Where to go next

- @ref your_first_scene "Your First Scene" adds model loading, input
  handling, physics bodies, and on-screen UI to what you just built.
- @ref scene_and_objects "Scenes, Objects, and Components" is the
  reference for the ownership model you have been using.
- @ref events_and_input "Events and Input" covers the dispatcher in full.
- @ref physics "Physics and Spatial Partitioning" covers colliders,
  rigidbodies, and world queries.
- @ref vertex_format "Vertex Format Registration" is what you need when the
  built-in vertex layout does not fit your geometry.
