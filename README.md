
<p align="center">
  <img src="logo.png" alt="SleakEngine" width="200">
</p>

<h1 align="center">SleakEngine</h1>

<p align="center">
  <strong>Open-source 3D game engine library built with C++23</strong>
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

---

## About

SleakEngine is the **core engine library** for building 3D applications. This repository contains only the engine itself &mdash; rendering, ECS, input, math, scene management, and all vendored dependencies.

To create a game project, use the **[SleakEngine-Empty](https://github.com/CanReader/SleakEngine-Empty)** starter template, which pulls this repo in as a git submodule.

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

## Templates

Get started quickly with a project template:

| Template | Description | Status |
|---|---|---|
| [Empty Template](https://github.com/CanReader/SleakEngine-Empty) | Minimal starter with a blank scene | Available |
| First Person Template | First-person camera and movement | Coming soon |
| Third Person Template | Third-person camera and character controller | Coming soon |
| Top Down Template | Top-down camera and controls | Coming soon |

## Using SleakEngine in Your Project

The recommended way is to add this repo as a git submodule:

```bash
git submodule add https://github.com/CanReader/SleakEngine.git Engine
git submodule update --init --recursive
```

Then in your root `CMakeLists.txt`:

```cmake
add_subdirectory(Engine)
```

Link against the `Engine` target from your game library.

Or use the **[SleakEngine-Empty](https://github.com/CanReader/SleakEngine-Empty)** template which has this all set up.

## Building Standalone

To build the engine library on its own:

```bash
git clone --recursive https://github.com/CanReader/SleakEngine.git
cd SleakEngine
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Repository Structure

```
SleakEngine/
└── Engine/
    ├── include/           Public & private headers
    ├── src/               Implementation
    ├── assets/shaders/    Default shaders
    └── vendors/           All third-party dependencies
```

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

## Contributing

Contributions are welcome. Fork the repository, create a feature branch, and open a pull request.

## License

SleakEngine is released under the [MIT License](LICENSE).
