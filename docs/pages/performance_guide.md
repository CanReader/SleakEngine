# Performance Guide {#performance_guide}

The deferred path is fragment bound. Almost every screen-space pass runs at
full swapchain resolution over the whole framebuffer, so the settings that
move frame time most are the ones that change how many pixels get shaded,
not the ones that change how many objects get submitted.

This guide covers what each setting actually costs, which backends
implement it, and how to measure the result instead of guessing.

---

## Start from a preset

`Sleak::GraphicsConfig::Preset` builds a full settings block from a named
tier. Take a preset, override the handful of fields your game cares about,
and push the result once.

```cpp
#include <Core/Application.hpp>
#include <Core/GraphicsConfig.hpp>

Sleak::GraphicsConfig cfg =
    Sleak::GraphicsConfig::Preset(Sleak::GraphicsQuality::Medium);
cfg.shadowMapResolution = 2048;
cfg.ssrEnabled = false;

if (auto* app = Sleak::Application::GetInstance())
    app->ApplyGraphicsConfig(cfg);
```

What the five tiers set:

| Tier | Shadows | Shadow map | SSAO | SSR | Bloom | IBL | TAA | Light shafts | Occlusion buffer |
|---|---|---|---|---|---|---|---|---|---|
| `Off` | off | n/a | off | off | off | off | off | off | disabled, 160x90 |
| `Low` | on, unfiltered | 1024 | off | off | off | off | off | off | disabled, 160x90 |
| `Medium` | on, filtered | 2048 | on | off | off | off | off | off | 256x144 |
| `High` | on, filtered | 2048 | on | on | on | on | off | on | 256x144 |
| `Ultra` | on, filtered | 3072 | on | on | on | on | on | on | 320x180, 256 occluders |

`Ultra` also widens the shadow range: distance 256, frustum half-extent
160, caster distance 160. Every other field keeps the `GraphicsConfig`
default, including `shadowDistance` 160, `shadowFrustumSize` 96, and
`msaaSamples` 1.

### What ApplyGraphicsConfig actually pushes

`Sleak::Application::ApplyGraphicsConfig` forwards a subset of the struct
to the renderer: SSAO toggle plus radius, bias, and power; SSR; bloom; IBL
toggle and intensity; TAA; shadow map resolution; MSAA sample count;
tonemapping, exposure, and gamma; and the PCSS flag. It stores the whole
config, so `GetGraphicsConfig()` reads back everything.

The rest of the struct is data your game applies itself. That is by design,
since those settings belong to systems the renderer does not own:

| Field group | Applied through |
|---|---|
| `shadowDistance`, `shadowFrustumSize`, `shadowBias`, `shadowStrength` | `GraphicsConfig::ApplyShadows(light)` on your `Sleak::DirectionalLight` |
| `shadowCasterDistance` | Your own shadow-caster culling |
| `frustumCullingEnabled`, `occlusionCullingEnabled`, `occlusionBufferWidth/Height`, `maxOccluders` | `Sleak::CullingSystem` setters |
| `renderScale` | Not consumed by any backend yet; treat it as reserved |
| `lightShaft*`, `fxaa*`, `sharpenStrength`, grading, `skyProcedural*` | Backend-specific setters where the backend implements them |

Forgetting the culling half is a common way to apply `Ultra` and see no
change in culling behavior at all.

---

## Know what your backend implements

Each backend reports a capability mask through
`Sleak::Application::GetGraphicsCaps()`. A setting the active backend does
not implement is accepted and ignored, so gate your settings UI on the mask
rather than assuming.

| Backend | Deferred | SSAO | SSR | TAA | Bloom | IBL | Shadows |
|---|---|---|---|---|---|---|---|
| Vulkan | yes | yes | yes | yes | yes | yes | yes |
| OpenGL | yes | yes | no | no | no | yes | yes |
| DirectX 11 | no | no | no | no | no | no | yes |
| DirectX 12 | no | no | no | no | no | no | yes |

Vulkan is the reference backend and the only one with the complete post
chain. DirectX 11 additionally implements a tonemap pass. See
@ref rendering_pipeline "Rendering Pipeline" for the per-backend detail.

Pick a backend with `-r`:

```bash
./bin/MyGame -r vulkan
./bin/MyGame -r opengl
```

Without the flag the platform default applies: DirectX 11 on Windows,
Vulkan everywhere else. Requesting DirectX 12 on a device that fails the
support check falls back to DirectX 11 with a warning.

---

## Cost characteristics, pass by pass

### Shadows

Shadow map resolution is the single biggest lever after resolution itself,
because the cost is quadratic: 3072 rasterizes 2.25 times the texels of
2048, which is 9 times the texels of 1024. It costs on both ends, once when
rasterizing the map and again when every lit pixel samples it.

The filter cost is separate and lives in the shaders. The deferred lighting
pass takes 16 PCF taps per lit pixel; the forward shaders
(`default_shader.frag`, `skinned_shader.frag`) take 48. Those counts are
compile-time constants, so the `Low` tier's "no filter" intent and the
`pcssEnabled` flag do not thin them out on the Vulkan path.

Cheaper before you drop resolution: shrink `shadowFrustumSize`. A tighter
frustum over the same map size raises texel density, so you can often keep
quality while halving resolution. Pair it with a `shadowDistance` large
enough that the near plane does not clip casters behind the camera.

### SSAO and SSR

Both allocate at full swapchain extent on Vulkan. They are the two passes
that scale hardest with window size, and on a fragment-bound frame they are
usually the first two things to try disabling when a resolution change
makes the frame unaffordable. SSAO also responds to `ssaoRadius`: a wide
radius spreads taps across more of the depth buffer and costs cache
locality, not just math.

### Bloom

Bloom already runs at half swapchain resolution through its downsample and
upsample chain, so it is the cheapest of the post effects to leave enabled.

### TAA

TAA is a full-resolution resolve plus a velocity buffer, so it is not free,
but it is the effect that buys the most stability per millisecond on scenes
with high-frequency shadow edges. It is Vulkan only.

### MSAA

MSAA and the deferred path are mutually exclusive. When deferred is active
the GBuffer is created with one sample per pixel and the swapchain forces
1x, and the OpenGL renderer skips its MSAA framebuffer entirely. Setting
`msaaSamples` to 4 with deferred enabled changes nothing visible and
nothing in frame time.

MSAA is worth reaching for on the DirectX backends, which run forward.
`Sleak::Application::GetMaxMSAASampleCount()` reports the device limit.

---

## Culling

`Sleak::CullingSystem` is CPU only and backend agnostic: view-frustum
culling plus software occlusion culling against a small depth buffer
rasterized from occluder volumes your game submits.

```cpp
Sleak::CullingSystem::SetOcclusionCullingEnabled(true);
Sleak::CullingSystem::SetOcclusionBufferSize(256, 144);
Sleak::CullingSystem::SetMaxOccluders(192);
Sleak::CullingSystem::SetAdaptiveOcclusion(true, 20);
```

Frustum culling is on by default and needs nothing from you: the main
camera calls `BeginFrame` during its update, and `IsVisible` degrades to a
frustum test when no occluders were submitted.

Occlusion culling is the part you opt into, and it has two invariants that
decide whether it helps or hurts:

- **Occluders must be fully solid volumes.** A box submitted over a cave, a
  doorway, or any interior air hides geometry that should be visible.
- **Test bounds must be tight around the real vertex extent.** Bounds that
  overshoot into empty sky pass the depth test, so you pay for the query
  and cull nothing.

The pass is adaptive. When a rasterized frame culls nothing it stops
rasterizing and probes again every `probeInterval` frames, falling back to
frustum-only in between, so an open scene where occlusion never pays does
not keep paying the rasterize cost. Read what it actually did:

```cpp
const auto& s = Sleak::CullingSystem::GetStats();
SLEAK_INFO("submitted {} rasterized {} | frustum {} occlusion {} | {:.2f}ms",
           s.occludersSubmitted, s.occludersRasterized, s.frustumCulled,
           s.occlusionCulled, s.rasterizeMs);
```

If `occlusionCulled` stays near zero while `rasterizeMs` is nonzero, the
occluders are not doing useful work: either they are too small to hide
anything, or the scene has no depth complexity to exploit.

### Render distance

Whatever draw distance your game exposes multiplies through everything
above: more objects tested, more geometry in the GBuffer, more casters in
the shadow pass. Cutting it is the bluntest and often the most effective
control you have, and unlike the post-effect toggles it reduces CPU work
too.

---

## Measure with the benchmark recorder

The engine ships a per-frame CSV recorder. `F12` toggles it at runtime, and
`--bench` (or `--benchmark`) starts recording the moment the loop begins.

```bash
./bin/MyGame -r vulkan -w 1920 -h 1080 --bench
```

Sessions land in `benchmarks/` next to the executable, one timestamped CSV
per run. Each row carries frame index, elapsed time, frame time in
milliseconds, FPS, triangles, CPU percentage, and RAM. The file ends with a
commented summary block:

| Summary field | What it tells you |
|---|---|
| `FPS_Min` / `FPS_Max` / `FPS_Avg` | Coarse shape of the run |
| `FrameTime_P50` / `P95` / `P99` | The numbers to compare between runs |
| `FrameTime_Stdev_ms` | Frame pacing consistency |
| `Spikes_16ms` / `33ms` / `50ms` | Frames that fell below 60, 30, and 20 FPS |
| `Triangles_Avg`, `CPU_Avg_%`, `RAM_Avg_MB` | Whether a change moved submission or the CPU side |
| `Renderer`, `VSync`, `MSAA`, system info | Enough context to compare two CSVs honestly |

Compare percentiles, not averages. An average hides exactly the spikes that
make a build feel bad.

Add your own columns for anything game-specific:

```cpp
#include <Debug/Benchmark.hpp>

if (auto* bench = Sleak::Application::GetInstance()->GetBenchmark()) {
    bench->RegisterMetric("RenderDistance",
                          [this]() { return (float)m_renderDistance; });
    bench->RegisterMetric("VisibleChunks",
                          [this]() { return (float)m_visibleChunks; });
}
```

Registered metrics appear as extra per-frame columns and get an `_Avg` line
in the summary. Unregister them before the owning object dies.

Turn VSync off while measuring, otherwise every result clamps to the
refresh rate and hides the change you are trying to see.

---

## A tuning order that works

1. Turn VSync off and record a baseline with `--bench`.
2. Halve the window resolution. If frame time roughly halves, you are
   fragment bound and the rest of this list applies. If it barely moves,
   the bottleneck is CPU or submission, and culling plus draw count is
   where to look instead.
3. Drop `shadowMapResolution` one step and tighten `shadowFrustumSize`.
4. Disable SSR, then SSAO. Both are full resolution.
5. Reduce render distance.
6. Re-record and compare P95 and P99, not the average.

---

## See also

- @ref rendering_pipeline "Rendering Pipeline" for the frame structure and
  per-backend feature matrix.
- @ref culling "Culling" for the occluder submission protocol in full.
- @ref building_shipping "Building and Shipping" for release builds and
  what to hand to testers.
