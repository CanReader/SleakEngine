# Lighting and Shadows {#lighting}

Lights are GameObjects. You add one to a scene the same way you add anything
else, and the scene's `Sleak::LightManager` picks it up, packs it into a
constant buffer, and hands it to the renderer once per frame. One directional
light in the scene can also cast a shadow map.

| Type | Role |
| :--- | :--- |
| `Sleak::Light` | Abstract base deriving from `Sleak::GameObject`; color, intensity, and shadow parameters. |
| `Sleak::DirectionalLight` | Infinite light with a direction; the only type that casts shadows. |
| `Sleak::PointLight` | Positioned light with a range. |
| `Sleak::SpotLight` | Positioned and directed light with inner and outer cone angles. |
| `Sleak::AreaLight` | Rectangle or disc emitter; shades as a point light today. |
| `Sleak::LightManager` | One per scene; collects lights, uploads them, drives the shadow matrices. |
| `Sleak::GraphicsConfig` | Data-driven shadow and post-processing settings. |

---

## 1. Adding a Light

```cpp
auto* sun = new Sleak::DirectionalLight("Sun");
sun->SetDirection({-0.4f, -1.0f, -0.3f});
sun->SetColor(1.0f, 0.96f, 0.9f);
sun->SetIntensity(2.5f);
sun->SetCastShadows(true);
AddObject(sun);
```

`SceneBase::AddObject` checks `GameObject::IsLight()` and calls
`LightManager::RegisterLight` for anything that answers yes, so there is no
separate registration step. The scene owns the light and deletes it;
`RemoveObject` and `DestroyObject` unregister it first.

`SetCastShadows` defaults to `false` on every light type. A scene with a
directional light and no shadows is usually a scene where nobody turned it
on.

Lights carry their own position rather than reading a
`Sleak::TransformComponent`. Move a point light with `SetPosition`.

---

## 2. Light Types

Every type shares the `Sleak::Light` base: `SetColor`, `SetIntensity`
(default 1.0), `SetEnabled` (default true), `SetCastShadows` (default false),
`SetShadowBias` (0.005), `SetShadowNormalBias` (0.02), `SetShadowStrength`
(1.0), and `SetLightSize` (1.0, the penumbra width knob for soft shadows).

**`Sleak::DirectionalLight`** takes a direction, normalized on assignment,
defaulting to straight down. Its extra settings shape the shadow volume:
`SetShadowFrustumSize` (160), `SetShadowDistance` (160),
`SetShadowNearPlane` (0.1), and `SetShadowFarPlane` (500). The frustum size
is the width of the orthographic box the shadow camera covers, and it is the
main lever on shadow sharpness: halving it doubles effective texel density
and halves how far shadows reach.

**`Sleak::PointLight`** takes a position and a range (default 10). There are
no attenuation coefficients; falloff is computed in the shader from the
range.

**`Sleak::SpotLight`** takes a position, a normalized direction, a range, and
inner and outer cone angles in **degrees** (20 and 30 by default). The angles
are converted to cosines when packed for the GPU.

**`Sleak::AreaLight`** takes a position, direction, range, width, height, an
`AreaLightShape` of `Rectangle` or `Disc`, and a two-sided flag. Its width
and height reach the GPU, but the shape and two-sided flags do not, and no
shader integrates over the emitter surface. An area light currently shades
exactly like a point light. Treat it as reserved.

---

## 3. How Lights Reach the GPU

\dot
digraph lightflow {
  bgcolor="transparent"; rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];
  add  [label="Scene::AddObject\nIsLight() -> RegisterLight"];
  upd  [label="SceneBase::Update\nLightManager::UpdateAndBind", fillcolor="#22d3ee22", color="#22d3ee"];
  cb   [label="LightCBData\nslot 2, up to 16 lights"];
  shadow [label="UpdateShadowData\npick shadow caster\nbuild lightVP, snap texels"];
  ubo  [label="ShadowLightUBO\n1 primary + 3 extra directionals"];
  fwd  [label="forward shaders\nall 4 light types"];
  def  [label="lighting_pass.frag\ndeferred, Vulkan"];
  add -> upd;
  upd -> cb -> fwd;
  upd -> shadow -> ubo;
  ubo -> fwd;
  ubo -> def;
}
\enddot

`SceneBase::Update` calls `LightManager::UpdateAndBind` once per frame, after
object updates so the camera matrices are current, and before the physics
step. That call fills two separate paths.

The first is `LightCBData`, a constant buffer at slot 2 holding camera
position, ambient, fog, and an array of up to `MAX_LIGHTS = 16` packed light
entries. Disabled lights are skipped. `RegisterLight` refuses to add a
seventeenth light and logs a warning. This buffer is what the forward shaders
read, and it is the only path that carries point, spot, and area lights.

The second is `ShadowLightUBO`, built by `UpdateShadowData`. It carries one
primary directional light plus up to **three** extra directional lights, the
light view-projection matrix, shadow bias and strength, texel size, and fog
parameters. This is what the shadow-aware paths read.

The split matters on Vulkan. `VulkanBuffer::Update()` is empty, constant
buffers are persistently mapped, and nothing binds the slot-2 buffer to a
Vulkan descriptor set. On Vulkan the engine effectively lights from
`ShadowLightUBO` alone, which means one primary directional light and up to
three additional directional lights. **Point, spot, and area lights
contribute nothing on the Vulkan backend.** They work on OpenGL and
DirectX 11, which bind the 16-light array.

Ambient defaults to `(0.1, 0.1, 0.1)` at intensity 1.0. Distance fog is
enabled by default, running from 80 to 256 units, with a height fog layer
topping out at 80 units. All of it is configurable on
`Sleak::LightManager`, though ambient color has a setter and no getter.

---

## 4. Deferred Lighting

On the Vulkan deferred path, geometry writes albedo and AO, normal and
roughness, and metallic and emissive into a three-target GBuffer plus depth.
`assets/shaders/lighting_pass.frag` then shades the whole screen with a
single fullscreen triangle, sampling the GBuffer, the shadow map, the SSAO
result, and the IBL cubemaps.

`LightManager::UpdateDeferredCB` supplies the pass with `InvViewProj`, screen
size, and near and far planes, so world position is reconstructed from depth
rather than stored. This is why the ordering inside `SceneBase::Update`
matters: objects update first so the camera matrices are current when
`InvViewProj` is built. A stale matrix here shows up as whole-scene shadow
flicker on camera rotation, not as a subtle offset.

`UpdateShadowData` also composes an `NdcToShadow` matrix on the CPU by
inverting the camera view-projection and multiplying by the light's, so the
lighting pass can go from screen space to shadow space in one transform.

---

## 5. Shadow Mapping

One directional light, one 2D depth map, no cascades. `LightManager` selects
the first enabled `Sleak::DirectionalLight` with `GetCastShadows()` true, in
registration order. If none casts, it still reads the first enabled
directional light for color and direction and tells the renderer to disable
the shadow pass.

The light view-projection is built each frame. The shadow camera follows the
camera's XZ position with its height anchored at zero, looks along the light
direction with a guard for near-vertical directions, and projects through a
left-handed orthographic box sized by `SetShadowFrustumSize` and
`SetShadowDistance`. The result is then snapped to whole shadow texels using
the live map resolution, which is what keeps shadow edges from crawling as
the camera moves. A `--shadowfreeze` command line flag latches the first
matrix forever, which is useful when you need to tell a projection bug apart
from a stability bug.

Resolution defaults to 2048 and clamps to the range 256 to 8192. Changing it
after the renderer has built its shadow resources works on Vulkan and is
silently queued and dropped on the other three backends.

Vulkan's shadow pass is vertex-only, using `shadow_depth.vert` with no
fragment stage, culling back faces, and applying a depth bias of 1.25
constant and 1.75 slope. It supports skinning, so animated characters cast
correctly posed shadows. The pass runs at the top of `BeginRender` against
the draw list cached from the previous frame, and when there is nothing
cached it leaves the previous frame's map intact rather than clearing it.

### Filtering

Sample counts are compile-time constants in the shaders. The Vulkan forward
path runs true PCSS: a 16-tap blocker search, a penumbra width derived from
the blocker distance and `SetLightSize`, and 48 PCF taps on a Vogel disk
rotated per pixel. The Vulkan deferred path drops the blocker search for a
fixed five-texel radius and 16 rotated taps, which is a deliberate trade
noted in the shader.

OpenGL and DirectX 11 branch on `GraphicsConfig::pcssEnabled`, taking a
16-tap blocker search plus 32 PCF taps when it is on and a 9-tap 3x3 box when
it is off. Vulkan and DirectX 12 never read that flag, so toggling it does
nothing there. DirectX 12 renders a shadow map its material shader never
samples, so nothing is shadowed on that backend. See @ref backend_support for
the full picture.

---

## 6. Quality Presets

`GraphicsConfig::Preset(GraphicsQuality)` builds a settings struct from
`Off`, `Low`, `Medium`, `High`, or `Ultra`. For shadows the tiers differ in
two fields: resolution (1024 on Low, 2048 in the middle, 3072 on Ultra) and
`pcssEnabled` (off for Off and Low, on above). Ultra also widens the shadow
distance to 256 and the frustum to 160. PCF sample counts do not change with
the tier.

Applying a config takes two calls, and missing the second is a common
surprise:

```cpp
auto cfg = Sleak::GraphicsConfig::Preset(Sleak::GraphicsQuality::High);
Sleak::Application::GetInstance()->ApplyGraphicsConfig(cfg);
cfg.ApplyShadows(*sun);
```

`ApplyGraphicsConfig` pushes the post-processing settings plus shadow map
resolution and `pcssEnabled` onto the renderer. The rest of the shadow block,
meaning `shadowEnabled`, `shadowDistance`, `shadowFrustumSize`,
`shadowBias`, and `shadowStrength`, lives on the light rather than the
renderer, and `ApplyShadows` is what copies it across.
`shadowCasterDistance` is deliberately left for the game's own culling.

---

## 7. Where to Look in the Source

| Question | File |
| :--- | :--- |
| Light base class and shared properties | `include/public/Lighting/Light.hpp` |
| Per-type parameters | `include/public/Lighting/{Directional,Point,Spot,Area}Light.hpp` |
| Collection, packing, and shadow matrices | `src/Lighting/LightManager.cpp` |
| Automatic registration from a scene | `SceneBase::AddObject` in `src/Scene/SceneBase.cpp` |
| GPU struct layouts and `MAX_LIGHTS` | `include/private/Graphics/Common/ConstantBuffer.hpp` |
| Vulkan shadow resources and pipeline | `src/Graphics/Vulkan/VulkanShadow.cpp` |
| Deferred lighting shader | `assets/shaders/lighting_pass.frag` |
| Forward shading and PCSS | `assets/shaders/default_shader.frag`, `default_shader_gl.frag` |
| Quality presets | `src/Graphics/Common/GraphicsConfig.cpp` |
