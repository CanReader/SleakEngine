# Assets and Resources {#assets_resources}

The engine has no asset database, no content pipeline, and no global cache.
Assets are files on disk, loaded on demand by whoever needs them, and owned
by the object that asked for them. That is a small surface to learn and a
sharp one to misuse, so the ownership rules matter more here than the API
does.

| Type | Role |
| :--- | :--- |
| `Sleak::Texture` | Backend-agnostic GPU texture interface; one implementation per renderer. |
| `Sleak::Material` | Shader plus seven texture slots plus PBR scalars, uploaded as one constant buffer. |
| `Sleak::ModelLoader` | Assimp import producing a GameObject hierarchy, or animation clips alone. |
| `Sleak::MeshBatch` | GPU mesh handles for bulk static geometry outside the component system. |
| `Sleak::MeshHandle` | Vertex buffer, index buffer, index count, and vertex format for one mesh. |
| `Sleak::RefPtr` | Shared-ownership pointer for GPU resources. |
| `Sleak::ObjectPtr` | Move-only unique ownership, used for material-owned resources. |

---

## 1. Paths and the Working Directory

Every asset path in the engine is a relative string rooted at `assets/`,
resolved against the process working directory. There is no virtual file
system, no search path list, and no mount points. The hardcoded paths in the
engine look like this:

```
assets/shaders/default_shader.hlsl
assets/textures/default_skybox.hdr
assets/branding/icon.bmp
```

That convention only works if the working directory contains `assets/`, which
is why a game's `main()` normally chdirs to the executable's own directory
before doing anything else. Launch the binary from elsewhere without that
step and the first thing to fail is shader loading.

Shader paths are a special case: the `.hlsl` name is a stem that each backend
rewrites into its own variant. See @ref shader_authoring.

### Staging assets next to the binary

The engine's CMake stages nothing but its own compiled SPIR-V (into
`assets/shaders` next to each executable that links `Engine`). Building the
runtime asset tree is the consuming project's job, and the shape that works
is per-file dependency tracking rather than a post-build directory copy:

```cmake
file(GLOB_RECURSE _files CONFIGURE_DEPENDS "${SRC_DIR}/*")
foreach(_src ${_files})
    file(RELATIVE_PATH _rel "${SRC_DIR}" "${_src}")
    add_custom_command(
        OUTPUT "${_asset_out_dir}/${_rel}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_asset_out_dir}/${_rel}"
        DEPENDS "${_src}")
endforeach()
add_custom_target(CopyAssets ALL DEPENDS ${SLEAK_ASSET_OUTPUTS})
add_dependencies(MyGame CopyAssets)
```

A `POST_BUILD` copy fires only when the executable relinks, so editing a
shader or a texture without touching a `.cpp` leaves the staged copy stale
and the change invisible. Tracking each file as its own output makes edits
propagate on every build. SleakCraft's `Client/CMakeLists.txt` is the worked
version, and it stages the game's asset tree first so a game file wins over
an engine file of the same name.

---

## 2. RefPtr and Ownership

`Sleak::RefPtr<T>` is the engine's shared pointer, and every GPU resource
handle uses it. Never substitute `std::shared_ptr` for an engine resource.

```cpp
Sleak::RefPtr<Sleak::RenderEngine::BufferBase> buffer(rawBuffer);
```

It holds a separately allocated control block with an atomic reference count,
so the count is safe to touch from multiple threads while the pointee is not.
Copying shares the block, moving transfers it, and the last owner deletes the
object. Upcasting from `RefPtr<Derived>` to `RefPtr<Base>` is supported and
shares the same block.

Four differences from `std::shared_ptr` are worth internalizing:

- **No `make_shared` equivalent.** You always write `RefPtr<T>(new T(...))`,
  which means two allocations per object.
- **Constructing two `RefPtr`s from the same raw pointer creates two
  independent control blocks**, and both will delete it. Pass the `RefPtr`
  around, never the raw pointer.
- **No custom deleter and no aliasing constructor.** The destructor always
  runs plain `delete`.
- **`WeakPtr` is not usable.** It exists in `include/public/Memory/` but
  references members `RefPtr` does not have and will not compile if
  instantiated. Nothing in the engine uses it.

`Sleak::ObjectPtr<T>` is the move-only counterpart, roughly a `unique_ptr`.
`Sleak::Material` uses it for the shader, the constant buffer, and its seven
textures, which is what makes a material the sole owner of everything it
loads.

For scene objects the rule is different again: the scene owns GameObjects,
and you release one with `SceneBase::DestroyObject` rather than `delete`.
See @ref scene_and_objects.

---

## 3. Textures

`Sleak::Texture` is a pure virtual interface; the concrete class comes from
the active backend. You rarely construct one.

```cpp
material->SetDiffuseTexture("assets/textures/crate_albedo.png");

if (Sleak::Texture* tex = material->GetDiffuseTexture()) {
    tex->SetFilter(Sleak::TextureFilter::Nearest);
    tex->SetWrapMode(Sleak::TextureWrapMode::ClampToEdge);
}
```

Filtering and wrapping are per texture, not per material. `TextureFilter`
runs from `Nearest` through `Bilinear` and `Trilinear` to `Anisotropic2x`
up to `Anisotropic16x`, with `Linear` and `Anisotropic` kept as aliases for
`Trilinear` and `Anisotropic16x`. `TextureWrapMode` covers `Repeat`,
`ClampToEdge`, `ClampToBorder`, `Mirror`, and `MirrorClampToEdge`.
`TextureFormat` covers `RGBA8`, `RGB8`, `BGRA8`, `DXT1`, and `DXT5`, and
`TextureType` covers `Texture2D`, `TextureCube`, and `Texture3D`.

**There is no texture cache.** No `TextureManager`, no dedup by path, no
reference counting across load calls. Loading the same PNG through two
materials decodes and uploads it twice. A game that shares textures widely
should keep its own map from path to `Sleak::Texture*` and hand the pointer
around.

Two paths sidestep materials. `Sleak::UI::LoadTextureForUI` caches by path
and returns a UI-bindable id, and `Sleak::UI::CreateTextureFromPixels` builds
a texture from raw RGBA for atlases assembled at runtime. See
@ref ui_system.

`Sleak::CreateTexture` is the full-control version of the latter. Describe
the texture with a `TextureDesc`: size, `RGBA8` or `BGRA8`, the `srgb` flag,
and `mipLevels`, where 0, the default, means the full chain down to 1x1. Then
pass either level 0 alone, and the backend builds the other levels, or every
level of the chain. Rows run top to bottom with no padding, and level `i` is
`GetMipExtent(width, i)` by `GetMipExtent(height, i)` texels. The pixel storage
behind each `TextureMipLevel` must stay alive until `CreateTexture` returns;
the texture keeps its own copy after that. The result is a `RefPtr<Texture>`
that is null on bad input or failure. Call it on the thread that drives the
renderer.

```cpp
Sleak::TextureDesc desc;
desc.width = 256;
desc.height = 256;
desc.srgb = true;  // color data, sampled as linear

// Level 0 alone: the backend generates the other mips
const Sleak::TextureMipLevel base{pixels.data(), pixels.size()};
Sleak::RefPtr<Sleak::Texture> texture = Sleak::CreateTexture(desc, &base, 1);

// Or every level, built by your own filter
std::vector<Sleak::TextureMipLevel> chain = BuildMyMips(pixels, desc);
texture = Sleak::CreateTexture(desc, chain.data(),
                               static_cast<uint32_t>(chain.size()));

if (texture.IsValid()) material->SetDiffuseTexture(texture);
```

Vulkan and OpenGL upload every supplied level as given and honour `srgb` and
`mipLevels`. DirectX 11 and 12 take level 0 only: they build their own full
chain, ignore `mipLevels` and `srgb`, reject `BGRA8` through `CreateTexture`,
and log a warning once. Filter, wrap mode and LOD bias still come from the
texture's setters; `SetNearestMagnification(true)` keeps magnified texels
sharp while `SetFilter` keeps driving minification and mips. DirectX ignores
`SetNearestMagnification`.

---

## 4. Materials

`Sleak::Material` bundles a shader, seven texture slots, and the PBR scalars
into a single constant buffer at slot 1.

Texture slots are fixed: diffuse 0, normal 1, specular 2, roughness 3,
metallic 4, AO 5, emissive 6. Scalars cover diffuse, specular, and emissive
colors, shininess (32), metallic (0), roughness (0.5), AO (1), normal
intensity (1), emissive intensity (1), opacity (1), alpha cutoff (0.5), plus
UV tiling and offset.

`MaterialRenderMode` selects `Opaque`, `Cutout`, or `Transparent`, and
`SetTwoSided` disables back-face culling. `IsForwardRendered()` is what the
deferred path checks to decide whether a draw belongs in the GBuffer pass or
the forward transparent pass.

A material owns its textures through `ObjectPtr`, so it frees them with
itself. Do not hand the same `Sleak::Texture*` to two materials.

---

## 5. Loading Models

`Sleak::ModelLoader::Load` imports through Assimp and returns a
`Sleak::GameObject*` you own.

```cpp
Sleak::ModelLoadOptions opts;
opts.scaleFactor = 0.01f;
opts.flipUVs = true;

if (auto* model = Sleak::ModelLoader::Load("assets/models/Mannequin.fbx", opts)) {
    AddObject(model);
}
```

`ModelLoadOptions` carries `scaleFactor`, `flipUVs` (true by default),
`flipNormals`, `flipWinding`, and an initial position and rotation. Format
support is whatever the vendored Assimp build provides, which covers FBX,
glTF, and OBJ among others. `Load` returns `nullptr` on failure after logging
the Assimp error.

The import runs base postprocessing of triangulate, generate smooth normals,
calculate tangent space, join identical vertices, and optimize meshes, plus
UV flipping when requested. It then reads the file once to check for
animations. A static model is re-imported with `PreTransformVertices` folded
in; an animated one keeps its node hierarchy so the skeleton and clips can be
extracted.

Each mesh in the file becomes a child GameObject carrying, in order, a
`TransformComponent`, a `MaterialComponent`, a `ColliderComponent` built from
the mesh as an AABB, a static `RigidbodyComponent`, and a `MeshComponent`.
**Colliders and static rigidbodies are added unconditionally with no opt-out
in `ModelLoadOptions`.** Strip them afterward if the model is decorative.

Materials come across with diffuse, specular, and emissive colors, shininess,
opacity, metallic, and roughness, and with diffuse, normal, specular,
emissive, metalness, and roughness textures. AO textures are not imported
even though the slot exists. Embedded textures are decoded from the file.
Shaders are assigned by hand: `skinned_shader.hlsl` for rigged meshes and
`default_shader.hlsl` otherwise.

Texture dedup inside an import is per call and keyed by path plus Assimp
texture type, so the cache dies when `Load` returns and a second `Load` of
the same file reloads everything.

`LoadAnimationsOnly(filePath, skeleton)` pulls clips out of a second file
against an existing skeleton, for the common case of a mesh in one file and
its animations in several others.

---

## 6. MeshBatch

When you have far more geometry than you want GameObjects for, such as
terrain chunks or voxel columns, `Sleak::MeshBatch` builds GPU buffers
directly and skips per-object component overhead.

```cpp
// Once, describing your own vertex layout
Sleak::VertexLayoutDesc desc;
desc.stride = sizeof(MyVertex);
desc.attributes = {
    {0, Sleak::VertexAttribFormat::Float3, offsetof(MyVertex, pos)},
    {1, Sleak::VertexAttribFormat::Float3, offsetof(MyVertex, normal)},
};
desc.shaderStem = "my_forward";
Sleak::VertexFormatHandle fmt = Sleak::VertexFormatRegistry::Register(desc);

// Per chunk
Sleak::MeshHandle mesh = Sleak::MeshBatch::CreateMesh(
    fmt, verts.data(), verts.size() * sizeof(MyVertex),
    indices.data(), indices.size());

// Per frame
Sleak::MeshBatch::BeginBatch(material);
for (const auto& chunk : visible) {
    if (chunk.mesh.IsValid()) Sleak::MeshBatch::Draw(chunk.mesh);
}
Sleak::MeshBatch::EndBatch();
```

The other `CreateMesh` overload takes a `VertexGroup` and `IndexGroup` and
uses the engine's built-in 96-byte `Sleak::Vertex` layout, leaving
`vertexFormat` at 0.

`Draw` takes a `castsShadow` flag defaulting to true; pass false for distant
geometry to keep it out of the shadow pass. `BeginBatch` binds the material
and a shared identity-transform buffer once, so every `Draw` in the batch
shares them.

For geometry that moves, build a `MeshComponent` from the handle instead,
`MeshComponent(object, mesh)`: the component draws the handle's buffers,
custom vertex format included, with the owner's `TransformComponent`, so a
moving object never rebuilds its buffers.

**There is no `DestroyMesh`.** A `MeshHandle` holds `RefPtr`s to its buffers,
and the GPU memory is freed when the last copy of the handle goes away. Drop
the handle, or overwrite it, and the buffers follow. `MeshBatch` does not
track handles for you, so the mesh lives exactly as long as you keep the
struct.

Every entry point touches renderer state, so call them from the thread that
drives the frame, and call `Shutdown()` before the renderer is torn down.

---

## 7. Serialization

`include/public/FileSystem/` holds a single header, `Serializable.hpp`. It
defines a `Serializable` interface with `Serialize` and `Deserialize` plus
`SerializeToFile` and `DeserializeFromFile`, an `ISerializationContext`
abstraction over key-value read and write, and a `SerializationFactory` that
detects `Binary`, `JSON`, `YAML`, or `XML` from a file extension.

Nothing in the engine's own asset types implements it. Meshes, materials,
textures, and animation clips exist only as imported runtime objects, so a
game that wants its own save format builds on `Serializable` directly.

---

## 8. Where to Look in the Source

| Question | File |
| :--- | :--- |
| Texture interface and sampling enums | `include/public/Runtime/Texture.hpp` |
| Material fields and slot constants | `include/public/Runtime/Material.hpp` |
| Assimp import, component attachment, texture cache | `src/Assets/ModelLoader.cpp` |
| Mesh handle lifetime and batch drawing | `include/public/Runtime/MeshBatch.hpp`, `src/Graphics/Common/MeshBatch.cpp` |
| Built-in `Vertex` layout | `include/public/Runtime/MeshData.hpp` |
| Reference counting semantics | `include/public/Memory/RefPtr.hpp` |
| Backend resource factory | `include/private/Graphics/Common/ResourceManager.hpp` |
| Serialization interfaces | `include/public/FileSystem/Serializable.hpp` |
