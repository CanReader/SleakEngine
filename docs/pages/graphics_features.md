# Graphics Features {#graphics_features}

A tracker of notable real-time graphics features, the kind found in high-end
shader packs and in large commercial engines, checked against what SleakEngine
actually ships.

<span class="feat-y">✓</span> green means implemented in SleakEngine and
verified in the source tree. <span class="feat-n">✓</span> grey means not
implemented. The notes column names the implementing file or class, and records
which backends carry the feature when it is not all four.

Backend shorthand: VK (Vulkan), GL (OpenGL), DX11, DX12. Vulkan is the most
complete backend; DX11 covers forward rendering with shadows and tonemapping,
DX12 covers forward rendering with shadows.

## Rendering paths

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-y">✓</span> | Deferred shading | `VulkanDeferred.cpp` GBuffer + `lighting_pass.frag`; GL path in `OpenGLRenderer.cpp` with `gbuffer_gl` / `lighting_pass_gl` |
| <span class="feat-y">✓</span> | Forward rendering | `Renderer::SetDeferredEnabled`, `default_shader` variants on all four backends |
| <span class="feat-y">✓</span> | Forward transparent pass | `VulkanRenderer::BeginForwardTransparentPass`, routed by `RenderCommandQueue.cpp` after lighting |
| <span class="feat-n">✓</span> | Forward+ / clustered shading | - |
| <span class="feat-n">✓</span> | Visibility buffer | - |

## Geometry

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-y">✓</span> | Static mesh rendering | `MeshComponent`, `Sleak::MeshData` |
| <span class="feat-y">✓</span> | Bulk static geometry batching | `Sleak::MeshBatch` handle pool, `BeginBatch` / `Draw` / `EndBatch` |
| <span class="feat-y">✓</span> | Model import (FBX, glTF, OBJ) | `Sleak::ModelLoader`, Assimp-backed, builds the GameObject tree |
| <span class="feat-y">✓</span> | Skeletal skinning | `Sleak::Skeleton` (256 bones), `skinned_shader` variants, forward pass only |
| <span class="feat-y">✓</span> | Hardware instancing | `RenderContext::DrawInstance` / `DrawIndexedInstance` on all four backends; instance data must come from `gl_InstanceIndex`, there is no per-instance attribute stream |
| <span class="feat-y">✓</span> | Custom / procedural vertex formats | `Sleak::VertexLayoutDesc` + `VertexFormatRegistry::Register`, consumed by `MeshBatch::CreateMesh` |
| <span class="feat-n">✓</span> | Morph targets / blend shapes | - |
| <span class="feat-n">✓</span> | Mesh LOD | no LOD selection; `LOD` in the tree refers to texture mip levels only |
| <span class="feat-n">✓</span> | Hierarchical LOD (HLOD) | - |
| <span class="feat-n">✓</span> | Tessellation | no hull/domain or tessellation control/evaluation stages |
| <span class="feat-n">✓</span> | Mesh shaders | - |
| <span class="feat-n">✓</span> | Virtualized geometry (Nanite class) | - |

## Lighting

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-y">✓</span> | Directional lights | `Sleak::DirectionalLight`, shadow-casting light picked in `LightManager::UpdateAndBind` |
| <span class="feat-y">✓</span> | Point lights | `Sleak::PointLight`, UE4-style inverse-square falloff in `default_shader_gl.frag` |
| <span class="feat-y">✓</span> | Spot lights | `Sleak::SpotLight`, inner/outer cone smoothstep |
| <span class="feat-y">✓</span> | Physically based shading | Cook-Torrance GGX + Smith + Fresnel-Schlick in `lighting_pass.frag` and `default_shader_gl.frag` |
| <span class="feat-y">✓</span> | Image-based lighting | `OpenGLIBL.cpp` bakes irradiance, prefiltered specular, and the split-sum BRDF LUT; the Vulkan side (`VulkanIBL.cpp`) allocates 1x1 black stubs with no bake, so GL only |
| <span class="feat-n">✓</span> | Area lights | `Sleak::AreaLight` packs GPU type 3, but no shader branches on it and it shades as a point light |
| <span class="feat-n">✓</span> | Baked lightmaps | - |
| <span class="feat-n">✓</span> | Light probes / reflection captures | - |
| <span class="feat-n">✓</span> | Dynamic global illumination (Lumen class) | - |
| <span class="feat-n">✓</span> | Screen space global illumination | - |
| <span class="feat-n">✓</span> | Distance field ambient occlusion | - |

## Shadows

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-y">✓</span> | Shadow mapping | `VulkanShadow.cpp`, `shadow_depth.vert`, hardware comparison sampler |
| <span class="feat-y">✓</span> | Runtime shadow map resolution | `Renderer::SetShadowMapResolution`, deferred apply through `ApplyShadowResolutionChange` |
| <span class="feat-y">✓</span> | PCF filtering | 12-tap rotated Vogel disk with interleaved gradient noise, `PCFFilter` in `lighting_pass.frag` |
| <span class="feat-y">✓</span> | PCSS / contact hardening | blocker search in `default_shader.frag`, `default_shader_gl.frag`, `skinned_shader.frag`; forward path only, deferred uses fixed-radius PCF |
| <span class="feat-n">✓</span> | Cascaded shadow maps | single shadow frustum, no cascade split |
| <span class="feat-n">✓</span> | Virtual shadow maps | - |
| <span class="feat-n">✓</span> | Ray traced shadows | - |
| <span class="feat-n">✓</span> | Contact shadows (screen space) | - |
| <span class="feat-n">✓</span> | Point / spot light shadows | only the directional light casts |

## Screen space and post processing

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-y">✓</span> | SSAO | `VulkanSSAO.cpp` and `ExecuteSSAOPass` in `OpenGLRenderer.cpp`, hemisphere kernel from `SSAOKernel.hpp`, separate blur pass |
| <span class="feat-y">✓</span> | Screen space reflections | `VulkanSSR.cpp`, `ssr.frag`; VK only |
| <span class="feat-y">✓</span> | Temporal anti-aliasing | `VulkanTAA.cpp`, `taa.frag`, Halton sub-pixel jitter applied to the WVP push constant; VK only |
| <span class="feat-y">✓</span> | MSAA | `Renderer::SetMSAASampleCount`, VK pipelines and GL `m_msaaFBO`; forward only, the deferred path silently forces 1 sample because a multisampled GBuffer is too expensive |
| <span class="feat-y">✓</span> | Bloom | `VulkanBloom.cpp`, threshold + downsample + upsample + composite chain at half resolution; VK only |
| <span class="feat-y">✓</span> | Tone mapping | ACES filmic (Narkowicz fit) in `tonemap.frag`, `tonemap_gl.frag`, `tonemap.hlsl`, `tonemap_dx12.hlsl` |
| <span class="feat-y">✓</span> | Exposure and gamma control | `PostProcessUBO`, `Renderer::SetExposure` / `SetGamma` |
| <span class="feat-y">✓</span> | HDR render target | `CapHDRTarget`, RGBA16F scene image through the lighting, bloom, and tonemap chain |
| <span class="feat-n">✓</span> | FXAA | `GraphicsConfig::fxaaEnabled` exists but no backend reads it |
| <span class="feat-n">✓</span> | Upscaling (TSR / DLSS / FSR class) | - |
| <span class="feat-n">✓</span> | Render scale | `GraphicsConfig::renderScale` is declared but unread |
| <span class="feat-n">✓</span> | HDR display output (HDR10 / scRGB) | swapchain selects `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` only |
| <span class="feat-n">✓</span> | Auto exposure / eye adaptation | config fields only, no luminance histogram or reduction pass |
| <span class="feat-n">✓</span> | Color grading / LUT | - |
| <span class="feat-n">✓</span> | Motion blur | - |
| <span class="feat-n">✓</span> | Depth of field | - |
| <span class="feat-n">✓</span> | Vignette | config field only, unread |
| <span class="feat-n">✓</span> | Chromatic aberration | - |
| <span class="feat-n">✓</span> | Film grain | config field only, unread |
| <span class="feat-n">✓</span> | Lens flare | - |
| <span class="feat-n">✓</span> | Velocity / motion vector buffer | `CapVelocity` is declared but no backend produces one; TAA reprojects from dilated depth |

## Atmosphere and environment

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-y">✓</span> | Cubemap skybox | `Sleak::Skybox` six-face constructor, `skybox.frag` |
| <span class="feat-y">✓</span> | Equirectangular panorama sky | `SkyboxMode::Panorama` |
| <span class="feat-y">✓</span> | Procedural gradient sky | `SkyboxMode::Gradient`, three-color vertical blend |
| <span class="feat-y">✓</span> | Distance fog | `LightManager::SetFogDistances`, `ApplyFog` in `lighting_pass.frag` |
| <span class="feat-y">✓</span> | Exponential height fog | `LightManager::SetHeightFogDensity` / `SetHeightFogFalloff`, stacks with distance fog |
| <span class="feat-n">✓</span> | Physical sky / atmospheric scattering | gradient sky only, no Rayleigh or Mie model |
| <span class="feat-n">✓</span> | Volumetric fog | analytic fog only, no froxel volume |
| <span class="feat-n">✓</span> | Volumetric clouds | - |
| <span class="feat-n">✓</span> | God rays / light shafts | `CapLightShaft` and the `lightShaft*` config fields are declared, no backend implements the pass |
| <span class="feat-n">✓</span> | Water rendering | game side; the engine supplies the transparent pass hook via `transparentShaderStem` |
| <span class="feat-n">✓</span> | Planar reflections | - |
| <span class="feat-n">✓</span> | Rain and weather effects | - |
| <span class="feat-n">✓</span> | Wind / foliage animation | - |

## Materials and texturing

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-y">✓</span> | Normal mapping | TBN basis in `gbuffer.frag` with Z reconstruction for two-channel compressed normals |
| <span class="feat-y">✓</span> | Metallic / roughness workflow | `Sleak::Material` factors and maps, packed into GBuffer RT1 and RT2 |
| <span class="feat-y">✓</span> | Ambient occlusion maps | `texAO` sampled in `gbuffer.frag`, combined with SSAO in the lighting pass |
| <span class="feat-y">✓</span> | Emissive maps | `texEmissive` times `emissiveFactor`, HDR intensity |
| <span class="feat-y">✓</span> | Alpha cutout | `MaterialRenderMode::Cutout` with `SetAlphaCutoff` |
| <span class="feat-y">✓</span> | Alpha-blended transparency | `MaterialRenderMode::Transparent`, routed to the forward transparent pass |
| <span class="feat-y">✓</span> | Mipmap generation | `VulkanTexture::GenerateMipmaps` blit chain, `glGenerateMipmap` on GL |
| <span class="feat-n">✓</span> | Parallax occlusion mapping | - |
| <span class="feat-n">✓</span> | Subsurface scattering | - |
| <span class="feat-n">✓</span> | Refraction / translucency | alpha blending only, no scene-color refraction |
| <span class="feat-n">✓</span> | Deferred decals | - |
| <span class="feat-n">✓</span> | Virtual texturing | - |
| <span class="feat-n">✓</span> | Texture streaming | textures upload whole at load time |
| <span class="feat-n">✓</span> | Texture atlasing | game side |
| <span class="feat-n">✓</span> | Anisotropic filtering | samplers use linear filtering without an anisotropy request |

## Ray tracing

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-n">✓</span> | Ray traced reflections | no acceleration structures in any backend |
| <span class="feat-n">✓</span> | Ray traced global illumination | - |
| <span class="feat-n">✓</span> | Ray traced ambient occlusion | - |
| <span class="feat-n">✓</span> | Ray traced shadows | - |
| <span class="feat-n">✓</span> | Path tracing | - |

## Performance and culling

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-y">✓</span> | View frustum culling | `Sleak::CullingSystem::IsVisible`, planes from `Sleak::ViewFrustum` |
| <span class="feat-y">✓</span> | CPU software occlusion culling | `CullingSystem.cpp` rasterizes submitted occluder volumes into a low-res depth buffer |
| <span class="feat-y">✓</span> | Data-driven quality presets | `GraphicsConfig::Preset`, applied in one shot by `Application::ApplyGraphicsConfig` |
| <span class="feat-y">✓</span> | Debug wireframe visualization | `Sleak::DebugLineRenderer` lines, AABBs, and spheres |
| <span class="feat-n">✓</span> | GPU occlusion queries | - |
| <span class="feat-n">✓</span> | GPU-driven rendering / indirect draw | no indirect draw entry points on `RenderContext` |

## Particles and effects

| Status | Feature | SleakEngine notes |
|:---:|---|---|
| <span class="feat-n">✓</span> | CPU particle system | - |
| <span class="feat-n">✓</span> | GPU particle system | - |
