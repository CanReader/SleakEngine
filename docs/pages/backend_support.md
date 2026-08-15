# Backend Feature Support {#backend_support}

SleakEngine runs on Vulkan, OpenGL, DirectX 11, and DirectX 12, selected at
launch with `-r`. The four are not equivalent. Vulkan carries the complete
feature set; the others implement a subset, and a game that exposes graphics
settings should gate them on what the active backend actually reports.

---

## 1. The Capability Mask

Each backend declares what it implements by overriding
`Renderer::GetFeatureCaps()`, which returns a `uint32_t` built from
`GraphicsCaps` bits:

```cpp
enum GraphicsCaps : uint32_t {
    CapDeferred      = 1 << 0,   CapSSAO          = 1 << 1,
    CapSSR           = 1 << 2,   CapTAA           = 1 << 3,
    CapBloom         = 1 << 4,   CapIBL           = 1 << 5,
    CapTonemapPass   = 1 << 6,   CapShadows       = 1 << 7,
    CapHDRTarget     = 1 << 8,   CapVelocity      = 1 << 9,
    CapLightShaft    = 1 << 10,  CapProceduralSky = 1 << 11
};
```

The base class returns `CapShadows` alone. Read the active backend's mask
through `Sleak::Application::GetGraphicsCaps()`.

`CapVelocity`, `CapLightShaft`, and `CapProceduralSky` are declared but set
by no backend. Treat them as reserved.

The mask is advisory. `Application::ApplyGraphicsConfig` pushes every field
of a `Sleak::GraphicsConfig` onto the renderer without consulting it, and a
backend that does not implement a feature simply ignores the setter. Nothing
throws and nothing warns, so a settings screen that offers SSR on DirectX 11
will look like it works and change nothing on screen. Check the mask before
you draw the checkbox.

---

## 2. The Matrix

| Feature | Vulkan | OpenGL | DirectX 11 | DirectX 12 |
| :--- | :---: | :---: | :---: | :---: |
| `GetFeatureCaps()` | `0x13F` | `0xA3` | `0xC0` | `0x80` |
| Deferred rendering | Yes | Declared, needs a game-supplied shader | No | No |
| Forward rendering | Yes | Yes | Yes | Yes |
| Shadow map (single directional) | Yes | Yes | Yes | Rendered, never sampled |
| PCSS soft shadows | Always on, forward path | Toggleable | Toggleable | No |
| `pcssEnabled` honored | No | Yes | Yes | No |
| Runtime shadow resolution change | Yes | No | No | No |
| SSAO | Yes, full res | With deferred only | No | No |
| SSR | Yes, full res | No | No | No |
| TAA | Yes | No | No | No |
| Bloom | Yes, half res | No | No | No |
| IBL | Yes, stub cubemaps by default | Yes | No | No |
| Separate tonemap pass | No, folded into bloom | No | Yes | No |
| HDR render target | Yes | No | No | No |
| MSAA implemented | Yes | Yes | Yes | No |
| MSAA reachable above 1x | No | No | No | No |
| Custom vertex attributes | Yes | Yes | No | No |
| Custom vertex shader stems | Yes | Ignored | Ignored | Ignored |
| Polygon mode / cull face switching | No | Yes | Yes | Yes |
| GPU skinning | Yes | Yes | Shader only | Shader only |

Approximate implementation weight: Vulkan around 12,200 lines across 16
files, OpenGL around 2,400 across 6, DirectX 11 around 2,400 across 5,
DirectX 12 around 3,000 across 5.

---

## 3. Shadows

Every backend renders a **single** directional shadow map. There are no
cascades anywhere in the engine, and only directional lights cast shadows.
`Sleak::LightManager` picks the first enabled `Sleak::DirectionalLight` whose
`GetCastShadows()` is true, in registration order.

Default resolution is 2048, settable through
`GraphicsConfig::shadowMapResolution` and clamped to the range 256 to 8192.
A change requested before the renderer creates its shadow resources applies
everywhere. A change requested afterward is queued into a pending field and
applied only by Vulkan, which is the sole backend overriding
`ApplyShadowResolutionChange()`. On the other three the request is stored and
never acted on, without a log line.

Filtering is fixed at compile time in the shaders, so quality presets change
resolution rather than sample count:

| Path | Blocker taps | PCF taps |
| :--- | :---: | :---: |
| Vulkan forward (`default_shader.frag`) | 16 | 48 |
| Vulkan deferred (`lighting_pass.frag`) | none, fixed radius | 16 |
| OpenGL, PCSS on (`default_shader_gl.frag`) | 16 | 32 |
| OpenGL, PCSS off | none | 9 |
| DirectX 11, PCSS on (`default_shader.hlsl`) | 16 | 32 |
| DirectX 11, PCSS off | none | 9 |

`GraphicsConfig::pcssEnabled` reaches the shader only on OpenGL and
DirectX 11, where it selects between the blocker-search path and a 3x3 box
filter. Vulkan and DirectX 12 never read it.

---

## 4. Per-Backend Notes

### Vulkan

The reference backend and the only one where the whole feature list is live.
Deferred is the default path: a GBuffer geometry pass, SSAO and its bilateral
blur, a fullscreen lighting pass, a forward pass for transparent, skinned,
and debug geometry, and then a post chain of TAA, SSR, bloom, and a composite
that applies ACES tonemapping and gamma. There is no separate tonemap pass,
which is why Vulkan does not set `CapTonemapPass`.

SSAO and SSR both run at full swapchain resolution. Bloom starts at half
resolution and mips down six levels.

Two Vulkan-specific gaps are worth planning around. `SetRenderDrawMode` and
`SetRenderCullFace` are no-ops, since changing polygon or cull mode requires
rebuilding a pipeline. And the IBL resources default to 1x1 black stub
images, so `CapIBL` contributes nothing until real cubemaps are supplied.

### OpenGL

Forward rendering is complete and solid. Deferred is a different story: the
GBuffer, its three render targets, and the SSAO chain are all implemented,
but `CreateGBufferResources` compiles `assets/shaders/lighting_pass_gl.frag`,
and the engine does not ship that file. Without it the pass logs
`Failed to compile deferred lighting pass shader!`, tears the GBuffer down,
and falls back to forward. SSAO goes with it, since
`CreateSSAOResources` requires a live GBuffer.

A game that wants OpenGL deferred supplies `lighting_pass_gl.frag` in its own
shader folder, which SleakCraft does. Because a project's assets are staged
over the engine's, a game-side file of that name is what the loader finds.

Unlike Vulkan, OpenGL reads a registered `Sleak::VertexLayoutDesc` for its
attribute pointers but ignores all four shader stem fields, binding the
layout to its own fixed programs instead.

### DirectX 11

Forward only, with a complete shadow map and the engine's only separate
tonemap pass (`CapTonemapPass`, using `assets/shaders/tonemap.hlsl`). The
tonemap pass is gated on the sample count being 1 or less, so it and MSAA are
mutually exclusive by design.

The MSAA implementation itself is complete, including a quality-level probe
and resource recreation, but it is unreachable at more than 1x for the reason
in section 5.

DirectX 11 ignores registered vertex formats entirely, so custom-format
geometry is interpreted through the built-in 96-byte `Sleak::Vertex` layout.

### DirectX 12

The narrowest backend. Forward only, no post-processing, no IBL, no MSAA
resources at all. It sets `CapShadows`, and the producer side is real: a
depth buffer, a DSV, a depth-only PSO, a shadow pass, an SRV bound at `t3`,
and a comparison sampler at `s3`. The consumer side is not.
`assets/shaders/default_shader_dx12.hlsl` declares only
`Texture2D diffuseTexture : register(t0)` and its sampler, and its pixel
shader computes ambient, Lambert, Blinn-Phong, and fog with no shadow term.
The map is rendered every frame and discarded, so nothing is shadowed on
screen.

DirectX 12 also ignores registered vertex formats, and several stems in the
engine's shader set have no `_dx12.hlsl` variant at all, including
`skinned_shader` and the three `ibl_*` stems.

When `DirectX12Renderer::IsSupport()` fails, the factory logs
`The GPU does not support DirectX 12, using DirectX 11 instead` and returns a
DirectX 11 renderer, so read the type back from `GetRendererTypeStr()` rather
than assuming the flag you passed took effect.

---

## 5. MSAA

`Renderer::SetMSAASampleCount` validates the count against 1, 2, 4, and 8,
clamps it to the hardware maximum, and then rejects anything above 1x while
deferred rendering is enabled. A multisampled GBuffer would mismatch the
1-sample attachments the deferred render passes are built with. The rejection
is silent: no log, no return value, and `GetMSAASampleCount()` keeps
reporting the previous value.

The catch is that `m_deferredEnabled` defaults to `true` on the base
`Renderer`, and `SetDeferredEnabled` has no callers anywhere in the engine.
The check therefore fires on every backend, including the two that have no
deferred path at all. Vulkan, OpenGL, and DirectX 11 each carry a working
`ApplyMSAAChange` implementation that nothing currently reaches, and
DirectX 12 has no MSAA support to reach.

Practically: treat MSAA as unavailable, and reach for TAA on Vulkan when you
need anti-aliasing. Expect `-msaa 4`, `GraphicsConfig::msaaSamples`, and the
debug overlay's MSAA dropdown to have no visible effect.

---

## 6. Custom Vertex Formats

Games that register a `Sleak::VertexLayoutDesc` through
`Sleak::VertexFormatRegistry` get correct attribute binding on Vulkan and
OpenGL. Only Vulkan reads the four shader stem fields and builds a pipeline
per pass from them. Neither DirectX backend references the registry, so
custom-format geometry is bound through the built-in vertex layout and
renders as garbage.

Registration failures are quiet. An unknown handle, a zero stride, or an
empty attribute list makes Vulkan's pipeline creation return early with no
log at all, and makes OpenGL fall through to the built-in layout with no
diagnostic. A named stem that fails to load does log, once, before the pass
is skipped permanently for that format. See @ref shader_authoring for the
stem-to-file mapping and the exact messages.

---

## 7. Where to Look in the Source

| Question | File |
| :--- | :--- |
| The `GraphicsCaps` bits and the base default | `include/private/Graphics/Common/Renderer.hpp` |
| Each backend's declared mask | `GetFeatureCaps` in each `include/private/Graphics/<backend>/` renderer header |
| The MSAA rejection | `Renderer::SetMSAASampleCount` in `Renderer.hpp` |
| Shadow resolution queueing | `Renderer::SetShadowMapResolution`, `ApplyShadowResolutionChange` |
| Deferred split and post chain | `src/Graphics/Vulkan/VulkanDeferred.cpp`, `VulkanBloom.cpp` |
| OpenGL GBuffer setup and its shader load | `src/Graphics/OpenGL/OpenGLRenderer.cpp`, `CreateGBufferResources` |
| DX11 tonemap pass | `src/Graphics/DirectX11/DirectX11Renderer.cpp`, `ExecuteTonemapPass` |
| Backend selection and the DX12 fallback | `src/Graphics/Common/RendererFactory.cpp` |
