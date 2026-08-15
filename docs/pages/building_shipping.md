# Building and Shipping {#building_shipping}

SleakEngine is consumed as source. There is no install step, no find
package, and no prebuilt binary: your project adds the engine directory as
a CMake subproject and builds it alongside your game. Every third-party
dependency is vendored, so a working compiler and CMake are the only
prerequisites.

| Requirement | Minimum |
|---|---|
| CMake | 3.31 |
| C++ standard | C++23 (MSVC, GCC, or Clang) |
| Everything else | Vendored in `vendors/` |

---

## Start from the template

`SleakEngine-Empty` is the recommended starting point. It is a GitHub
template repository that already contains the three-target layout, working
CMake wiring, presets, and a minimal scene that renders a lit cube with a
skybox and a free-look camera.

```
YourGame/
├── CMakeLists.txt         adds Engine, Game, Client in that order
├── CMakePresets.json      debug and release presets
├── Engine/                SleakEngine, as a git submodule
├── Game/
│   ├── include/           Game.hpp, Scenes/EmptyScene.hpp
│   ├── src/               Game.cpp, Scenes/EmptyScene.cpp
│   └── assets/            textures, skybox, your own shaders
├── Client/
│   └── src/main.cpp       entry point, asset staging lives in its CMake
└── scripts/               build and shader-compile helpers
```

Use the template button rather than forking, then clone with submodules:

```bash
git clone --recurse-submodules https://github.com/you/YourGame.git
cd YourGame
git submodule update --remote Engine
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive --remote
```

The template also ships container build files and a GitHub Actions workflow
that bumps the `Engine` submodule when the engine repository publishes an
update.

---

## Or check the engine out as a sibling

A submodule is not the only option. `Engine/` only has to be a directory
CMake can `add_subdirectory` into, so a symlink to a sibling checkout works
and is the better setup when you edit the engine and the game together in
one session.

```bash
git clone https://github.com/CanReader/SleakEngine.git ../SleakEngine
ln -s ../SleakEngine Engine
```

With a symlink there is no gitlink, no `.gitmodules`, and no engine SHA
pinned in your history. `git -C Engine <command>` operates directly on the
sibling checkout, and keeping it current is a manual `git -C Engine pull`.
The tradeoff is real: nothing records which engine revision a given game
commit was built against, so pin a tag or move to a submodule before a
release you may need to reproduce.

---

## Wire up CMake

The root list file sends every artifact to one `bin/` directory and adds
the three subprojects in dependency order.

```cmake
cmake_minimum_required(VERSION 3.31)
project(YourGame LANGUAGES CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 23)

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} /utf-8")
endif()

add_subdirectory(Engine)
add_subdirectory(Game)
add_subdirectory(Client)
```

`Engine` builds as a shared library and detects that it is not the top
level project, so it leaves your output directories and standard settings
alone. The `/utf-8` flag has to go through `CMAKE_CXX_FLAGS` rather than
`add_compile_options` so the vendored Assimp build detects it and does not
add a conflicting `/source-charset:utf-8`.

The one rule your game's target must honor is the include path: only
`Engine/include/public` is yours to use. `Engine/include/private` and
`Engine/src` are engine internals and are not stable.

```cmake
target_include_directories(SleakGame PUBLIC
                           ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_SOURCE_DIR}/Engine/include/public)
target_link_libraries(SleakGame PRIVATE Engine)
target_compile_definitions(SleakGame PRIVATE SLEAK_EXPORTS)
```

Dear ImGui is a `PRIVATE` dependency of the engine target on purpose.
Including `imgui.h` from game code may link on an incremental Linux build
and will fail on a clean or Windows build. Use `UI/UI.hpp`.

Full listings for all three CMake files are in
@ref getting_started "Getting Started".

---

## Build with presets

```bash
cmake --preset debug && cmake --build --preset debug
cmake --preset release && cmake --build --preset release
```

Both presets configure into `build/` and write binaries to `bin/`. A clean
rebuild is `rm -rf build bin` followed by a configure and build.

Debug is the build to develop against: it is where assertions and any
`#ifdef DEBUG` developer flags in your game are live. Release is what you
measure and ship.

---

## Asset staging

Asset paths in code are relative to the executable's directory, and the
entry point sets the working directory to that location on startup, so
`"assets/skybox/right.jpg"` resolves to `bin/assets/skybox/right.jpg`.

The `Client` target is what puts the files there. Both asset trees are
copied into one directory next to the binary after every build:

```cmake
add_custom_command(TARGET YourGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Engine/assets
        $<TARGET_FILE_DIR:YourGame>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Game/assets
        $<TARGET_FILE_DIR:YourGame>/assets
    COMMENT "Copying assets to output directory")
```

Order matters when both trees contain a file with the same name: the last
copy wins. Copy your game assets second if you intend to override an engine
default, first if you do not.

**Editing an asset is not enough; you have to rebuild.** And a `POST_BUILD`
command only runs when the executable relinks, so editing a shader or a
texture without touching a `.cpp` can leave `bin/assets` stale. If that
bites you, replace the two `copy_directory` calls with per-file custom
commands driven by a `CONFIGURE_DEPENDS` glob, and hang them off an `ALL`
target the executable depends on. That version tracks each asset as its own
dependency and propagates edits on every build.

---

## Shaders

Vulkan consumes SPIR-V, so GLSL sources have to be compiled before the
binary can use them. The engine's own CMake handles its own folder: if
`glslc` is found it adds a `CompileShaders` target that compiles
`Engine/assets/shaders/*.vert` and `*.frag` to `.spv` and makes `Engine`
depend on it. When `glslc` is missing it prints a status message and skips
the step, so a build can succeed with stale SPIR-V.

That target is scoped to the engine's shader folder. It does not see
`Game/assets/shaders`, so any shader your game owns needs a manual compile
step. The template's `scripts/compile_shaders.sh` covers both directories:

```bash
./scripts/compile_shaders.sh
```

A shader has up to four backend variants, and they are looked up by
filename stem:

| Backend | Filenames |
|---|---|
| Vulkan | `name.vert`, `name.frag`, plus the compiled `.spv` |
| OpenGL | `name_gl.vert`, `name_gl.frag`, compiled at runtime |
| DirectX 11 | `name.hlsl` |
| DirectX 12 | `name_dx12.hlsl` |

When you change a shader, grep for the stem and update every variant you
ship. Commit the `.spv` files: a machine without `glslc` still needs them.

---

## What to distribute

A build directory contains far more than a player needs. Ship the contents
of `bin/`, minus anything your build left behind:

| Item | Notes |
|---|---|
| The executable | `bin/YourGame`, or `YourGame.exe` |
| Engine and game shared libraries | `Engine` and `SleakGame` both build shared |
| `bin/assets/` | The merged engine and game asset tree, including `.spv` |
| Runtime DLLs (Windows) | Staged for you by the `TARGET_RUNTIME_DLLS` copy step |

The executable changes its working directory to its own location at
startup, so the launcher, shortcut, or store client can start it from
anywhere as long as `assets/` sits beside it.

Do not ship `build/`, `benchmarks/`, or the source trees.

---

## Platform notes

**Linux** is the primary development platform. Vulkan is the default
backend and OpenGL is the fallback. The engine links a vendored Vulkan
loader, and the build fails at configure time if that library is missing
from `vendors/VulkanSdk`. OpenGL comes from the system through
`find_package(OpenGL REQUIRED)`.

**Windows** adds the two DirectX backends, which are compiled only on
Windows. DirectX 11 is the default backend there; DirectX 12 falls back to
DirectX 11 with a warning when the device fails its support check. The
`if(WIN32)` block in the root CMake steers per-configuration output
directories into the same `bin/`, which matters for multi-config
generators, and the `TARGET_RUNTIME_DLLS` post-build step puts the vendored
DLLs next to the executable.

Verify a release candidate on more than one backend before shipping it. The
backends diverge in what they implement, and a scene tuned on Vulkan can
look wrong on a forward backend that never runs the post chain. See
@ref rendering_pipeline "Rendering Pipeline" for the capability matrix.

---

## See also

- @ref getting_started "Getting Started" for the complete CMake and entry
  point listings.
- @ref performance_guide "Performance Guide" for measuring a release build.
- @ref release_notes "Release Notes" for what a given engine version
  contains.
