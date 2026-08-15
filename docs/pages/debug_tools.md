# Debug Tools {#debug_tools}

The engine ships four diagnostic facilities that need no setup beyond
`Sleak::Logger::Init`: leveled logging, a command line table, an in-game
overlay with wireframe collider drawing, and a CSV benchmark recorder.
`Sleak::Application` wires the last two up on its own, so a game gets them
by existing.

| Type | Role |
| :--- | :--- |
| `Sleak::Logger` | spdlog console and file sinks behind the `SLEAK_*` macros. |
| `Sleak::CommandLine` | Process-wide flag and value table parsed once in `main()`. |
| `Sleak::DebugOverlay` | Camera and performance panels, plus the collider-wireframe toggle. |
| `Sleak::DebugLineRenderer` | Immediate-mode wireframe lines, boxes, spheres, and capsules. |
| `Sleak::SystemMetrics` | OS-level CPU, RAM, and GPU usage snapshots. |
| `Sleak::Benchmark` | Per-frame CSV recorder with percentiles, spike counts, and custom metrics. |

---

## 1. Logging

```cpp
SLEAK_LOG(...)    // trace, the quietest level
SLEAK_INFO(...)
SLEAK_WARN(...)
SLEAK_ERROR(...)
SLEAK_FATAL(...)  // critical
SLEAK_RETURN_ERR(...)  // logs at error level, then returns false
```

Messages use fmt-style `{}` placeholders, never printf specifiers. This is
the opposite convention from `Sleak::UI::Text`, which is printf-style, so a
`%d` that compiles cleanly in a UI call will throw a format error in a log
call.

```cpp
SLEAK_INFO("Loaded {} chunks in {:.2f} ms", count, elapsedMs);
```

`Logger::Init("MyGame")` runs once from `main()` before anything logs. It
creates a colored console sink and a file sink, then registers two loggers
behind them. The macros pick between the two automatically: engine
translation units compile with `SLEAK_ENGINE` defined and resolve to
`Logger::GetCoreLogger()`, while game code resolves to
`Logger::GetLogger()`. Both write to the same sinks, so the split shows up
as a logger name in the output rather than a second file to watch.

`SLEAK_RETURN_ERR` suits initialization paths that return `bool`:

```cpp
bool LoadLevel(const std::string& path) {
    if (!Exists(path)) SLEAK_RETURN_ERR("Missing level {}", path);
    return true;
}
```

---

## 2. Command Line

`Sleak::CommandLine::Parse(argc, argv)` fills a static table read from
anywhere afterward. Call it in `main()` before constructing
`Sleak::Application`, which reads its own settings out of that table;
skipping the call silently disables every launch flag.

```cpp
static std::string GetValue(const std::string& flag, const std::string& defaultVal = "");
static bool        HasFlag(const std::string& flag);
static void        SetHelpCallback(void(*callback)(const char* exe));
```

Single-dash flags carry a value (`-r vulkan`), double-dash flags are
booleans (`--fullscreen`). Flags the engine acts on itself:

| Flag | Effect |
| :--- | :--- |
| `-r <vulkan\|opengl\|d3d11\|d3d12>` | Selects the backend; otherwise DirectX 11 on Windows, Vulkan elsewhere. |
| `-w <n>`, `-h <n>` | Window size. |
| `-t <title>` | Window title, with `_` standing in for spaces. |
| `--fullscreen` | Enters fullscreen once the window exists. |
| `--bench` or `--benchmark` | Starts benchmark recording as soon as the loop opens. |
| `--help` or `help` | Invokes the callback registered with `SetHelpCallback`, then keeps parsing. |

Everything else is yours. Games declare nothing in advance and just read
`GetValue("-seed")` or `HasFlag("--fly")` from their own code, which is why
a game's `--help` text is its own function rather than something the engine
prints.

### Graphics validation

The engine does not gate validation behind a flag. Every Vulkan run
enumerates the instance layers and enables `VK_LAYER_KHRONOS_validation` if
it is installed, logging `Vulkan validation layer enabled` at info level.
When the layer is missing it logs a warning, drops
`VK_EXT_debug_utils` from the extension list, and continues, which is the
case where a `VK_ERROR_DEVICE_LOST` arrives with no explanation attached.
A game that advertises its own `--validate` switch is describing behavior
the engine already performs unconditionally.

---

## 3. The Debug Overlay

`Application` constructs a `Sleak::DebugOverlay`, calls its `Initialize`,
and renders it every frame after `GameBase::Loop`. Both of its panels start
disabled, so nothing appears until the game opts in:

```cpp
auto& cfg = overlay->GetConfig();
cfg.ShowPerformancePanel = true;
```

`DebugOverlayConfig` carries `ShowCameraPanel`, `ShowPerformancePanel`,
`PanelAlpha` (0.85 by default), and `MetricRefreshInterval` (0.5 seconds).
The interval throttles `SystemMetrics::Query` only; frame counters update
every frame. `SetVisible`, `IsVisible`, and `ToggleVisible` gate the whole
overlay above the per-panel switches, and rendering also short-circuits
when the active renderer reports ImGui disabled.

The camera panel reads the active scene's camera and prints its name,
position, direction, look target, and up vector, with a drag control for
field of view. The performance panel prints the backend name in its accent
color, then FPS, frame time, vertex and triangle counts, cached CPU and RAM
figures, GPU usage when the platform reports it, a collider wireframe
checkbox, and an MSAA dropdown clamped to the backend's
`GetMaxMSAASampleCount()`.

### Engine hotkeys

`Application::OnKeyPressed` claims three keys before the game sees them:

| Key | Effect |
| :--- | :--- |
| `F9` | Toggles the active camera's `FirstPersonController`, or its `FreeLookCameraController` when there is no first-person one. |
| `F11` | Toggles fullscreen. |
| `F12` | Toggles benchmark recording. |

---

## 4. Wireframe Debug Drawing

`Sleak::DebugLineRenderer` is an all-static queue flushed once per frame.
`DebugOverlay::Initialize` allocates its buffers and shader, so it is ready
whenever the overlay exists, but `SetEnabled` defaults to `false` and the
queue draws nothing until something turns it on. The performance panel's
"Show Colliders" checkbox is the usual switch.

```cpp
static void SetEnabled(bool enabled);
static void DrawLine(const Math::Vector3D& start, const Math::Vector3D& end,
                     float r, float g, float b, float a = 1.0f);
static void DrawAABB(const Physics::AABB& aabb, float r, float g, float b, float a = 1.0f);
static void DrawSphere(const Math::Vector3D& center, float radius,
                       float r, float g, float b, float a = 1.0f, int segments = 16);
static void DrawCapsule(const Physics::BoundingCapsule& capsule,
                        float r, float g, float b, float a = 1.0f, int segments = 16);
```

`SceneBase::Update` calls `Flush(m_activeCamera)` at its end, after object
updates and the physics step, so anything queued during a component's
`Update` reaches the same frame. Immediately before the flush, and only
while the renderer is enabled, the scene walks its objects and draws every
`ColliderComponent` shape in green at its world transform. That walk is the
reason enabling line drawing costs something even when the game queues
nothing itself.

The shared vertex buffer holds `MAX_VERTICES = 65536` vertices, which is
32,768 line segments per frame.

---

## 5. Benchmark Recording

`Sleak::Benchmark` writes one CSV row per frame while recording. Start and
stop it with `F12`, with `--bench` on the command line, or by calling
`ToggleRecording()` on the instance from `Application::GetBenchmark()`.
Recording alone does not create any scene, so an automated run needs
whatever flags the game uses to enter a level as well.

Files land in a `benchmarks/` directory beside the working directory, named
`benchmark_<backend>_<YYYYMMDD>_<HHMMSS>.csv`. The per-frame columns are:

```
Frame,Time_s,FrameTime_ms,FPS,Triangles,CPU_%,RAM_MB[,custom metrics...]
```

On stop, a summary block follows as `#`-prefixed lines: frame count and
duration, backend, VSync and MSAA state, min/max/average FPS, frame time
min, max, average, P50, P95, P99, and standard deviation, spike counts past
16 ms, 33 ms, and 50 ms, and average triangles. The percentiles and the
spike counters are what separate a run that averages 60 FPS smoothly from
one that averages 60 FPS in bursts.

### Custom metrics

A game appends its own columns by registering a getter, which the recorder
samples once per frame and averages into the summary:

```cpp
auto* bench = Sleak::Application::GetInstance()->GetBenchmark();
bench->RegisterMetric("RenderDistance", [this] { return (float)m_renderDistance; });
bench->RegisterMetric("LoadedChunks",   [this] { return (float)m_chunks.size(); });
```

Register before recording starts; the header row is written at that moment.
`UnregisterMetric(name)` removes one when the object behind the lambda goes
away.

---

## 6. System Metrics

`Sleak::SystemMetrics::Query()` returns a `SystemMetricsData` with
`CpuUsagePercent`, `RamUsageMB`, `GpuUsagePercent`, and `GpuMemoryUsedMB`,
read from PDH counters on Windows and `/proc` on Linux. Platforms with no
implementation return zeros, which is why the overlay prints `GPU: N/A`
rather than `0.0%` when the reading is absent. `DebugOverlay::Initialize`
and its destructor own the `Initialize`/`Shutdown` pair, so games calling
`Query` directly need no setup of their own.

For renderer-side numbers, go through `Sleak::Application` instead:
`GetFPS`, `GetFrameTime`, `GetVertices`, `GetTriangles`,
`GetRendererTypeStr`, `GetGPUMemoryUsed`, `GetGPUMemoryBudget`, and
`GetGraphicsCaps` for the active backend's feature mask.

---

## 7. Where to Look in the Source

| Question | File |
| :--- | :--- |
| Log macros, levels, and logger selection | `include/public/Core/Logger.hpp` |
| Flag parsing rules | `include/public/Core/CommandLine.hpp`, `src/Core/CommandLine.cpp` |
| Hotkeys and `--bench` auto-start | `src/Core/Application.cpp` |
| Overlay panels and the collider checkbox | `src/Debug/DebugOverlay.cpp` |
| Line queue, shapes, and the flush | `src/Debug/DebugLineRenderer.cpp`, `SceneBase::Update` in `src/Scene/SceneBase.cpp` |
| CSV columns, summary block, filename | `src/Debug/Benchmark.cpp` |
| Per-platform counter reads | `src/Debug/SystemMetrics.cpp` |
| Vulkan validation layer setup | `src/Graphics/Vulkan/VulkanDevice.cpp` |
