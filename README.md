<p align="center">
  <img src="logo.png" alt="SleakEngine" width="200">
</p>

<h1 align="center">SleakEngine</h1>

<p align="center">
  <strong>Open-source 3D game engine built with C++23</strong>
  <br />
  Multi-backend rendering &bull; Component-based ECS &bull; Cross-platform
</p>

<p align="center">
  <a href="https://github.com/CanReader/SleakEngine"><img src="https://img.shields.io/github/stars/CanReader/SleakEngine?style=for-the-badge&color=f5c211" alt="Stars"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue?style=for-the-badge" alt="License"></a>
  <a href="https://github.com/CanReader/SleakEngine"><img src="https://img.shields.io/badge/C%2B%2B-23-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++23"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/DirectX_11-supported-4a9b4a?style=flat-square" alt="DirectX 11">
  <img src="https://img.shields.io/badge/DirectX_12-supported-4a9b4a?style=flat-square" alt="DirectX 12">
  <img src="https://img.shields.io/badge/OpenGL-supported-4a9b4a?style=flat-square" alt="OpenGL">
  <img src="https://img.shields.io/badge/Vulkan-supported-4a9b4a?style=flat-square&logo=vulkan&logoColor=white" alt="Vulkan">
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey?style=flat-square" alt="Platform">
</p>

<p align="center">
  <a href="#features">Features</a> &bull;
  <a href="#quick-start">Quick Start</a> &bull;
  <a href="#architecture">Architecture</a> &bull;
  <a href="#building-your-game">Building Your Game</a> &bull;
  <a href="docs/SCENE_SYSTEM_GUIDE.md">Docs</a>
</p>

---

## Why SleakEngine?

SleakEngine gives you a clean, modular foundation for 3D applications without the bloat. The engine separates concerns into independent shared libraries &mdash; swap the renderer, replace the game layer, or extend the core without touching unrelated code.

Pick your graphics API at startup. Write your game logic once. Ship on any platform.

## Features

**Rendering**
- Four graphics backends behind a single abstract interface &mdash; DirectX 11, DirectX 12, OpenGL, Vulkan
- Factory-pattern renderer selection at runtime
- Deferred command queue for batched, order-independent draw submission

**Entity-Component System**
- GameObjects composed of pluggable Components (Transform, Mesh, Material, Camera, and custom)
- Full component lifecycle: `Initialize` &rarr; `OnEnable` &rarr; `Update` / `FixedUpdate` / `LateUpdate` &rarr; `OnDisable` &rarr; `OnDestroy`
- Parent-child hierarchies with recursive transforms
- Tag-based object queries and deferred destruction safe for use during update loops

**Scene Management**
- State-driven scene lifecycle: Unloaded &rarr; Loading &rarr; Active &rarr; Paused &rarr; Unloading
- Scenes own their objects &mdash; automatic cleanup on unload
- Seamless scene switching with `SetActiveScene()`

**Engine Core**
- Fixed timestep physics at 60 Hz, decoupled variable-rate rendering
- SDL3-based windowing and input across platforms
- spdlog-backed logging with severity macros (`SLEAK_LOG`, `SLEAK_WARN`, `SLEAK_ERROR`, `SLEAK_FATAL`)
- ImGui debug overlay with camera controls, performance metrics, and scene inspection
- Custom smart pointers (`RefPtr<T>`, `ObjectPtr<T>`, `WeakPtr<T>`) and containers (`List<T>`, `HashTable<K,V>`, `Queue<T>`, `Graph<T>`)

## Quick Start

### Prerequisites

| Requirement | Minimum |
|---|---|
| **CMake** | 3.31+ |
| **C++ Compiler** | C++23 (MSVC, GCC, or Clang) |

All dependencies are vendored &mdash; no package manager needed.

### Clone & Build

```bash
git clone https://github.com/CanReader/SleakEngine.git
cd SleakEngine
cmake --preset debug
cmake --build --preset debug
```

That's it. CMake handles all vendor compilation (SDL3, yaml-cpp, etc.), asset copying, and shared library deployment automatically via [CMake Presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html).

For an optimized build, use the `release` preset:

```bash
cmake --preset release
cmake --build --preset release
```

Output goes to `bin/` with all assets and runtime libraries in place, ready to run.

### Launch

```bash
./bin/SleakEngine -w 1280 -h 720 -t My_Game
```

| Flag | Description |
|---|---|
| `-w` | Window width |
| `-h` | Window height |
| `-t` | Window title (use `_` for spaces) |

## Architecture

SleakEngine is split into three independent build targets:

| Target | Role |
|---|---|
| **Engine** | Core shared library &mdash; rendering, ECS, input, math, scene management |
| **Game** | Game logic shared library &mdash; inherits from `Sleak::GameBase` |
| **Client** | Thin executable entry point &mdash; instantiates the game and runs the application |

```
Engine (core)           Game (your code)           Client (entry point)
┌──────────────┐       ┌──────────────┐           ┌──────────────┐
│ Renderer     │       │ GameBase     │           │ main()       │
│ ECS          │◄──────│ Scenes       │◄──────────│ Creates Game │
│ SceneManager │       │ GameObjects  │           │ Runs App     │
│ Input        │       │ Components   │           └──────────────┘
│ Math / Utils │       └──────────────┘
└──────────────┘
```

### Main Loop

```
Application::Run(GameBase*)
 │
 ├─ Game::Initialize()         // Create scenes, objects, components
 ├─ Game::Begin()              // Post-initialization setup
 │
 └─ Per Frame:
     ├─ Window::Update()       // Poll input, handle events
     ├─ Renderer::BeginRender()
     ├─ FixedUpdate()          // 60 Hz physics tick (accumulator-based)
     ├─ Update(deltaTime)      // Per-frame game logic
     ├─ LateUpdate(deltaTime)  // Post-update (camera follow, UI, etc.)
     └─ Renderer::EndRender()  // Flush render command queue
```

### Project Layout

```
SleakEngine/
├── CMakePresets.json      Build presets (debug / release)
├── Engine/
│   ├── include/           Public & private headers
│   ├── src/               Implementation
│   ├── assets/shaders/    Default shaders
│   └── vendors/           All third-party dependencies
├── Game/
│   ├── include/           Game headers
│   ├── src/               Game implementation
│   └── assets/            Textures, models, etc.
├── Client/
│   └── src/               main.cpp
└── docs/                  Documentation
```

## Building Your Game

Implement three methods in `Game/src/Game.cpp`:

```cpp
#include "Game.hpp"

void Game::Initialize() {
    auto* scene = CreateScene("MainScene");

    auto* player = scene->CreateObject("Player");
    player->AddComponent<Transform>();
    player->AddComponent<Mesh>();
    player->AddComponent<Material>();

    auto* camera = scene->CreateObject("Camera");
    camera->AddComponent<Transform>();
    camera->AddComponent<Camera>();

    SetActiveScene(scene);
}

void Game::Begin() {
    // Runs once after initialization
}

void Game::Loop(float DeltaTime) {
    // Runs every frame
}
```

See the **[Scene System Guide](docs/SCENE_SYSTEM_GUIDE.md)** for the complete API reference.

## Vendored Dependencies

| Library | Purpose |
|---|---|
| [SDL3](https://www.libsdl.org/) | Windowing, input, platform abstraction |
| [glm](https://github.com/g-truc/glm) | Mathematics |
| [spdlog](https://github.com/gabime/spdlog) / [fmt](https://github.com/fmtlib/fmt) | Logging |
| [Dear ImGui](https://github.com/ocornut/imgui) | Debug UI |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON serialization |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | YAML configuration |
| [glad](https://glad.dav1d.de/) | OpenGL loading |

No external downloads required &mdash; everything lives under `Engine/vendors/`.

## Contributing

Contributions are welcome. Fork the repository, create a feature branch, and open a pull request.

## Screenshots

<p align="center">
  <img src="Minimal-Scene.png" alt="SleakEngine — Minimal Scene" width="720">
  <br />
  <em>Minimal scene &mdash; debug camera, ImGui overlays, and real-time performance metrics</em>
</p>

## License

SleakEngine is released under the [MIT License](LICENSE).
