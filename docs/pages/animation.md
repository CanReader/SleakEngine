# Animation {#animation}

SleakEngine implements GPU skeletal animation: skeletons and clips imported
from model files, pose evaluation on the CPU, a bone matrix palette uploaded
per frame, and skinning in the vertex shader. A state machine with typed
parameters and cross-fade transitions sits on top.

The scope stops there. There is no sprite animation, no root motion, no IK,
no retargeting, no blend trees, no animation layers or masks, and no
animation events. What follows documents what exists.

| Type | Role |
| :--- | :--- |
| `Sleak::Skeleton` | Bone list with inverse bind matrices, plus the imported node tree. |
| `Sleak::AnimationClip` | Named position, rotation, and scale keyframe tracks per node. |
| `Sleak::AnimationStateMachine` | States, parameter-driven transitions, and cross-fade weights. |
| `Sleak::AnimatorComponent` | Samples clips, builds the bone palette, uploads it to slot 3. |
| `Sleak::ModelLoader` | Imports skeleton, clips, and the animator in one call. |

---

## 1. Getting Something Animated

`Sleak::ModelLoader::Load` does the whole setup when the file contains
animations. It builds the skeleton, extracts every clip, and attaches a
`Sleak::AnimatorComponent` to each rigged mesh.

```cpp
auto* model = Sleak::ModelLoader::Load("assets/models/Mannequin.fbx");
AddObject(model);

Sleak::AnimatorComponent* anim = nullptr;
for (size_t i = 0; i < model->GetChildren().GetSize(); ++i) {
    if (auto* a = model->GetChildren()[i]->GetComponent<Sleak::AnimatorComponent>()) {
        anim = a;
        break;
    }
}
if (anim) {
    anim->Play("Idle", /*loop=*/true);
    anim->SetSpeed(1.0f);
}
```

The loader puts each mesh in its own child GameObject, so the animator lives
on a child rather than on the returned root, and there is no
search-the-hierarchy helper. It is added only when the import found bones and
at least one clip.
For the common split where the mesh lives in one file and the motions in
others, load the animations separately against the skeleton you already have:

```cpp
auto clips = Sleak::ModelLoader::LoadAnimationsOnly(
    "assets/animations/Walking.fbx", anim->GetSkeleton());
for (auto* clip : clips) anim->AddClip(clip);
```

Clips loaded this way are renamed to the file stem when the embedded name is
empty or contains `Armature`, `mixamo`, or `Animation`, which cleans up
Mixamo exports and will also rename a clip you deliberately called
`Animation_Intro`.

---

## 2. Skeleton

`Sleak::Skeleton` is a flat bone array plus the full imported node tree.

A `Bone` carries a name, an id, a parent id, and an `offsetMatrix`, which is
the inverse bind pose taking mesh space to bone space. A `NodeData` carries a
name, a default local transform, a bone index or -1, and child indices. The
bind pose is not stored separately; it lives implicitly in the offset
matrices and the node default transforms.

Pose evaluation walks the **node tree**, not the bone parent links. A node
with no matching animation channel contributes its `defaultTransform`, which
is what lets a clip animate a subset of the rig.

`MAX_BONES` is 256 and `MAX_BONE_INFLUENCE` is 4, matching the four bone ids
and four weights in `Sleak::Vertex`. The limits are declared but not enforced
on the CPU side: `AddBone` will accept a 257th bone, and the shaders skip any
bone id at or above 256. Check `GetBoneCount()` after import if your rigs run
large. `GetBone` and `GetNode` do no bounds checking.

---

## 3. Clips

An `Sleak::AnimationClip` is a name, a duration in ticks, a ticks-per-second
rate, and a vector of channels. Each `AnimationChannel` is a bone name plus
three independent keyframe tracks for position, rotation, and scale.

```cpp
float seconds = clip->GetDurationInSeconds();   // duration / ticksPerSecond
```

`ticksPerSecond` falls back to 25.0 when the source file reports zero or
less. Channels are looked up by name through `FindChannel`, backed by a hash
map that `BuildLookup()` populates. The import calls it. If you append to the
public `channels` vector yourself, call it again or the lookup goes stale.

Interpolation is not part of the clip. Position and scale are lerped
componentwise and rotation uses a shortest-arc slerp with an nlerp fast path,
all inside `Sleak::AnimatorComponent`. There are no per-channel interpolation
modes, no curve editing, no compression, and no keyframe events. Keyframe
search is a linear scan per channel per frame with no cursor caching, so very
long clips on very dense rigs are the case to profile first.

---

## 4. The State Machine

`Sleak::AnimationStateMachine` drives which clip plays and blends between
two of them during a transition. Create one through the animator, which then
takes priority over `Play`.

```cpp
auto* sm = anim->CreateStateMachine();

int idle = sm->AddState("Idle", idleClip, /*loop=*/true);
int run  = sm->AddState("Run",  runClip,  /*loop=*/true);
sm->SetDefaultState(idle);

int toRun = sm->AddTransition(idle, run, /*blendDuration=*/0.2f);
sm->AddTransitionCondition(toRun, "speed", Sleak::CompareOp::Greater, 0.1f);

int toIdle = sm->AddTransition(run, idle, 0.25f);
sm->AddTransitionCondition(toIdle, "speed", Sleak::CompareOp::LessEqual, 0.1f);

// Per frame, from your controller
sm->SetFloat("speed", velocity.Length());
```

Parameters are a `std::variant` over `bool`, `float`, and `int`, set through
`SetBool`, `SetFloat`, and `SetInt`. Conditions compare a named parameter
against a threshold with `Equal`, `NotEqual`, `Greater`, `GreaterEqual`,
`Less`, or `LessEqual`. All values are coerced to float for the comparison,
and equality uses an epsilon of 0.001.

Conditions on a transition are ANDed. A transition with **no** conditions is
always true, which makes it an unconditional link rather than a disabled one.
A condition naming a parameter that was never set fails. Transitions are
scanned in the order they were added, and the first whose conditions pass
fires.

`AddTransition` also takes `waitForClipEnd`, which holds the transition until
the current clip finishes.

Three behaviors to design around:

- **Transitions are not interruptible.** While a blend is in flight the
  machine returns early and evaluates no transitions, so a blend cannot be
  redirected before it completes. Keep blend durations short for responsive
  controls.
- **A `blendDuration` of zero divides by zero.** `AddTransition` accepts it.
  Use a small positive value for an instant cut.
- **`Play()` is inert once a state machine exists.** The animator checks the
  machine first and returns before the single-clip path.

There are no blend trees, blendspaces, layers, masks, additive blending,
phase syncing, or state entry and exit callbacks. `SampleRequest`, the
machine's per-frame output, carries at most two clips and a blend weight.

---

## 5. How Bone Matrices Reach the Shader

\dot
digraph animflow {
  bgcolor="transparent"; rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];
  sm   [label="AnimationStateMachine::Update\nSampleRequest: clipA, clipB, weight"];
  samp [label="sample node hierarchy\nlerp / slerp keyframes"];
  pal  [label="bone palette\noffset * global * globalInverse", fillcolor="#22d3ee22", color="#22d3ee"];
  cb   [label="bone constant buffer\nslot 3"];
  mesh [label="MeshComponent\nAddConstantBuffer"];
  bind [label="RenderContext::BindBoneBuffer\nVulkan set 1 b0, GL binding 3"];
  vs   [label="skinned_shader.vert\nshadow_depth.vert\ngbuffer.vert"];
  sm -> samp -> pal -> cb -> mesh -> bind -> vs;
}
\enddot

`AnimatorComponent::Initialize` allocates a constant buffer sized to the
skeleton's bone count, marks it slot 3, and registers it with the sibling
`Sleak::MeshComponent`. If there is no mesh component on the same GameObject
it logs a warning and the skinning never reaches the GPU.

Each frame, `Update` asks the state machine for a `SampleRequest`, evaluates
one or two poses by walking the node tree, blends them if a transition is in
flight, and uploads the result.

Draw submission routes slot 3 specially: `RenderContext::BindBoneBuffer`
rather than the generic constant buffer bind. Vulkan binds it as descriptor
set 1 binding 0, copying into a per-frame 16 KB uniform buffer sized for the
full 256-matrix array. OpenGL binds it at uniform block binding 3.

`BindBoneBuffer` is a no-op in the base `RenderContext`, and neither DirectX
backend overrides it. `skinned_shader.hlsl` exists and declares its bone
constant buffer at `b3`, but there is no C++ plumbing behind it, so **skeletal
animation runs on Vulkan and OpenGL only**.

Skinning is applied in three vertex shaders: `skinned_shader.vert` for
forward, `gbuffer.vert` for the deferred geometry pass, and
`shadow_depth.vert` for the shadow pass, so animated characters cast
correctly posed shadows. All three fall back to an identity transform when
the summed bone weights are near zero, which lets unskinned vertices pass
through unchanged.

---

## 6. Known Limits

Worth knowing before you build on this:

- **Blending interpolates matrix elements, not transforms.** Cross-fades
  lerp the 4x4 matrices componentwise rather than blending translation,
  rotation, and scale separately. Bones shrink and skew mid-transition, more
  visibly the further apart the two poses are. Short blend durations hide
  most of it.
- **Skeletons and clips are never freed.** `ModelLoader` allocates them and
  the animator's destructor releases only its state machine. A game that
  loads models repeatedly leaks them. With a multi-mesh rigged model, every
  mesh's animator shares one skeleton and one clip vector.
- **The bone buffer is sized to the bone count, not to 256.** Vulkan copies
  into its own full-size buffer and is unaffected. OpenGL binds the
  undersized buffer directly against a shader declaring 256 matrices.
- **No animation serialization.** Clips exist only as imported runtime
  objects.

---

## 7. Where to Look in the Source

| Question | File |
| :--- | :--- |
| Bones, node tree, and the bone limits | `include/public/Runtime/Skeleton.hpp` |
| Clip and channel layout | `include/public/Runtime/AnimationClip.hpp` |
| States, transitions, and conditions | `include/public/Runtime/AnimationStateMachine.hpp`, `src/Animation/AnimationStateMachine.cpp` |
| Pose evaluation, interpolation, blending | `src/Animation/AnimatorComponent.cpp` |
| Component API | `include/public/ECS/Components/AnimatorComponent.hpp` |
| Skeleton and clip extraction from a file | `src/Assets/ModelLoader.cpp` |
| Bone buffer binding per backend | `RenderContext::BindBoneBuffer` overrides in `src/Graphics/Vulkan/VulkanDescriptors.cpp`, `src/Graphics/OpenGL/OpenGLRenderer.cpp` |
| Skinning in the shaders | `assets/shaders/skinned_shader.vert`, `shadow_depth.vert`, `gbuffer.vert` |
