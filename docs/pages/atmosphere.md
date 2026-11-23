# Procedural Atmosphere {#atmosphere}

The atmosphere draws a gradient sky with sun and moon discs, stars and a
glare around the light, fogs the scene toward a sky-matched color and fades
the edge of the drawn world into the sky. It is opt-in: a game that never
calls `Sleak::Application::SetAtmosphere` renders as before, skybox and
`LightManager` fog included.

```cpp
#include <Runtime/Atmosphere.hpp>

void MyScene::Update(float dt) {
    // Your clock, your palette, your world
    Sleak::AtmosphereSettings sky = EvaluateMySky();
    Sleak::Application::GetInstance()->SetAtmosphere(sky);
    Scene::Update(dt);
}
```

Call it before `Scene::Update`, every frame or whenever the values change.
The settings stay until the next scene activates, so a menu scene gets no
sky from the scene before it.

---

## 1. Who Owns What

The engine owns the shape of the model: gradient falloffs, the soft clip,
disc sizes and halos, the star grid and its brightness law, the fog curve.
Those are constants in `sleak/atmosphere.glsl`. The game owns everything
that follows its clock, its palette or its world, and passes it in
`Sleak::AtmosphereSettings` in linear scene units:

| Group | Fields |
| :--- | :--- |
| `sky` | `color`, `dayScale`, `tintColor`, `tintStrength`, `tintExponent`, `nightColor`, `dayWeight`, `groundDensity` |
| `sun` | `direction`, `color`, `intensity` (0 hides it), `reddenGain` |
| `moon` | `direction`, `tangent` (its direction of travel), `color` (black hides it), `phase` (0 full, 0.5 new) |
| `stars` | `color` (black hides them), `axis` and `angle` of the sky's turn |
| `glare` | `direction`, `color` (black is off), `width` |
| `fog` | `densityPerMeter` (0 is off), `exponent`, `heightEnabled`, `baseHeight`, `dayWeight`, `indoorColor`, `outdoor` |
| `border` | `start` and `end` of the edge fade, as horizontal distance; `end <= start` is off |

Directions are unit world vectors pointing toward the body. A default
constructed `AtmosphereSettings` is a black sky without fog: there is no
preset, a game fills every field it wants to see.

---

## 2. Where It Draws

`Application::IsAtmosphereActive()` tells whether the active backend draws
the atmosphere this frame. While it does, the scene's `Sleak::Skybox` is
not submitted; everywhere else the skybox draws as usual, so a game can
keep one as the sky of the other paths.

| Path | Sky and fog |
| :--- | :--- |
| Vulkan, deferred | A fullscreen composite over the HDR scene after the forward pass, at final depth, before TAA |
| OpenGL, deferred | The lighting stem: `SleakLightingDefaultMain` draws sky pixels and fogs lit ones, and a game stem does the same with `SleakAtmosphereSkyDisplay`, `SleakAtmosphereComposite` and `SleakAtmosphereApply` |
| Forward (deferred off), DirectX | Not drawn; the skybox and `LightManager` fog stay |

Turn `LightManager` fog off while the atmosphere is active, or the scene is
fogged twice; the engine warns once when it sees both.

---

## 3. Reflections

While the atmosphere draws, the scene-read sky slot (see
@ref vertex_format "transparent formats that read the scene") holds a sky
probe in place of the skybox cubemap: a 64 x 64 cube with 6 mips of the sky
gradient, without discs, stars or glare. It is stored like the scene
snapshot of the same backend, scene-linear on Vulkan and display-referred
on OpenGL, and redrawn every frame. `SkyValid` is 0 on the frames where the
slot still holds the other sky.

---

## 4. Shaders

`sleak/atmosphere.glsl` holds the block (`uAtmosphere`, Vulkan set 2 binding
4, OpenGL buffer 15) and the model: `SleakAtmosphereEnabled`,
`SleakAtmosphereSkyGradient`, `SleakAtmosphereSky`,
`SleakAtmosphereFogColor`, `SleakAtmosphereFogTransmittance` and
`SleakAtmosphereScatter`. `sleak/atmosphere_pass.glsl` adds the screen-space
entry points on top of `FrameData`. See @ref shader_authoring.

Cost on Vulkan: one blended fullscreen pass on the HDR target, which skips
fog-free pixels, plus the probe's six small passes and five blits; both are
timed in the `atmosphere` row of `--gpuprofile`.
