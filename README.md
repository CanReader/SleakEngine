<p align="center"><img src="docs/images/logo-256.png" width="128" alt="SleakEngine"/></p>
<h1 align="center">SleakEngine</h1>
<p align="center">C++23 game engine with four graphics backends: Vulkan, OpenGL, DirectX 11, DirectX 12.</p>

SleakEngine is a game engine library you link into your own project. You write a game
class, register scenes, populate them with objects and components, and the engine runs
the window, render loop, lighting, physics, culling, and input.

## Building

```bash
cmake -B build -S .
cmake --build build -j
```

Requires a C++23 compiler and CMake. Vulkan and OpenGL backends build everywhere;
DirectX 11/12 build on Windows.

## Documentation

Run `doxygen Doxyfile` in the repo root and open `docs/html/index.html`:
a Getting Started guide, subsystem programming guides, and the full C++ API reference.

## Games built on SleakEngine

SleakCraft (voxel sandbox), SleakSims, SleakEngine-FPP. Start your own from the
SleakEngine-Empty template.
