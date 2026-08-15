# Culling {#culling}

`Sleak::CullingSystem` (`include/public/Culling/CullingSystem.hpp`, ~500
lines of implementation in `src/Culling/CullingSystem.cpp`) is a CPU
visibility system: view-frustum culling plus software occlusion culling
against a low-resolution depth buffer rasterized from game-submitted
occluder volumes. It is backend-agnostic and does no GPU work. It is a
separate system from `Sleak::Physics::DynamicAABBTree`
(see @ref physics); neither references the other anywhere in the engine.

| Type | Role |
| :--- | :--- |
| `Sleak::CullingSystem` | All-static visibility system; frame protocol, occluder buffer, visibility tests. |
| `Sleak::CullingSystem::Stats` | Per-frame counters: submitted, rasterized, tested, culled, rasterize time. |
| `Sleak::ViewFrustum` | Six planes extracted from a view-projection matrix; the frustum test itself. |
| `Sleak::Math::AABB` | The box type every submission and visibility test speaks in. |

\dot
digraph cullflow {
  bgcolor="transparent"; rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];
  cam   [label="Camera::RecalculateViewMatrix\nViewFrustum::ExtractFromVP"];
  begin [label="BeginFrame(frustum, VP, cameraPos)\nclear occluders, reset Stats", fillcolor="#22d3ee22", color="#22d3ee"];
  sub   [label="SubmitOccluderBox\nSubmitOccluderTriangles\noptional, frustum-rejected early"];
  fin   [label="FinalizeOccluders()\nsort by distance, rasterize\nup to MaxOccluders", fillcolor="#22d3ee22", color="#22d3ee"];
  depth [label="occlusion depth buffer\n256x144 by default"];
  vis   [label="IsVisible(box)\nfrustum test, then depth test"];
  fonly [label="IsVisibleFrustumOnly(box)\nfrustum test only"];
  draw  [label="submit draw"];
  cam -> begin -> sub -> fin -> depth;
  depth -> vis [label="conservative test"];
  begin -> fonly [label="no occluders needed"];
  vis -> draw [label="true"];
  fonly -> draw [label="true"];
}
\enddot

The camera opens the frame, the game fills the depth buffer with solid
occluders, and every visibility test after `FinalizeOccluders` reads that
buffer.

---

## 1. Frame Protocol

```cpp
static void BeginFrame(const ViewFrustum& frustum, const Math::Matrix4& viewProj,
                        const Math::Vector3D& cameraPos);            // once per frame, after camera update
static void SubmitOccluderBox(const Math::AABB& box);                 // world-space, must be a fully solid volume
static void SubmitOccluderTriangles(const Math::Vector3D* vertices, uint32_t vertexCount,
                                     const uint32_t* indices, uint32_t indexCount);
static void FinalizeOccluders();                                       // sorts by distance, rasterizes up to the budget

static bool IsVisible(const Math::AABB& box);              // frustum test, then conservative occlusion depth test
static bool IsVisibleFrustumOnly(const Math::AABB& box);    // frustum test only, skips the occlusion buffer
```

Occluder submission (steps 2 and 3) is optional; `IsVisible` degrades to
frustum-only culling if no occluders were submitted that frame.
`BeginFrame` is called automatically by the engine's active camera
(`src/Scene/Camera.cpp`, `Camera::RecalculateViewMatrix` builds
`VP = View * Projection`, refreshes the static `s_frustum` through
`ViewFrustum::ExtractFromVP`, then calls
`CullingSystem::BeginFrame(s_frustum, VP, Position)`). `MeshComponent`
(`src/Scene/MeshComponent.cpp`) calls `CullingSystem::IsVisibleFrustumOnly`
on its owner's world bounds before submitting a draw, so frustum culling is
already wired into the default component draw path; full occlusion culling
(submitting occluder boxes/triangles) is left to the game.

`IsVisible` never falsely culls a box that is actually visible, given valid
occluders (conservative test).

---

## 2. Configuration and Diagnostics

```cpp
static void SetFrustumCullingEnabled(bool enabled);
static void SetOcclusionCullingEnabled(bool enabled);
static void SetOcclusionBufferSize(uint32_t width, uint32_t height);  // default 256x144
static void SetMaxOccluders(uint32_t count);                            // default 192, applied after the distance sort
static void SetAdaptiveOcclusion(bool enabled, uint32_t probeInterval);  // default on, interval 20

struct Stats {
    uint32_t occludersSubmitted = 0;
    uint32_t occludersRasterized = 0;
    uint32_t tested = 0;
    uint32_t frustumCulled = 0;
    uint32_t occlusionCulled = 0;
    bool occlusionSkipped = false;  // adaptive pass idle this frame
    float rasterizeMs = 0.0f;
};
static const Stats& GetStats();
static const float* GetDepthBuffer(uint32_t& width, uint32_t& height);  // row-major, NDC depth, debug only
static void Shutdown();
```

Adaptive occlusion skips rasterization for `probeInterval` frames once a
rasterized frame culls nothing, then probes again; queries degrade to
frustum-only while a skip is in effect. This keeps a mostly-static camera
from paying the rasterization cost every frame while still catching new
occlusion when the camera moves.

Occluder boxes and triangles submitted through
`SubmitOccluderBox`/`SubmitOccluderTriangles` must represent fully solid
volumes; an occluder placed over open space or a cave mouth would
incorrectly hide geometry behind it.

Two more properties are worth knowing before you submit occluders. Both
submission calls reject anything outside the frustum before it reaches the
queue, so submitting the whole world costs a plane test per volume rather
than a rasterization. `FinalizeOccluders` then sorts what survived by
squared distance to the camera and rasterizes only the nearest
`MaxOccluders` of them, which means a badly scoped submission loop loses
the far occluders rather than the near ones.

Bounds accuracy matters as much as solidity. An occludee box larger than
the geometry it stands for pokes past the real silhouette, every depth
sample behind it passes, and the object is never culled while still paying
the test. Keep submitted bounds at exact vertex extent.

---

## 3. Where to Look in the Source

| Question | File |
| :--- | :--- |
| The full frame protocol and rasterizer | `src/Culling/CullingSystem.cpp` |
| The public API and `Stats` layout | `include/public/Culling/CullingSystem.hpp` |
| Plane extraction and the frustum test | `include/public/Camera/ViewFrustum.hpp` |
| Where `BeginFrame` is called from | `src/Scene/Camera.cpp` |
| The default frustum-culled draw path | `src/Scene/MeshComponent.cpp` |
