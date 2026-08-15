#ifndef _CULLING_SYSTEM_HPP_
#define _CULLING_SYSTEM_HPP_

#include <Camera/ViewFrustum.hpp>
#include <Core/OSDef.hpp>
#include <Math/AABB.hpp>
#include <Math/Matrix.hpp>
#include <Math/Vector.hpp>
#include <cstdint>

namespace Sleak {

/// CPU visibility system: view-frustum culling plus software occlusion
/// culling against a low-resolution depth buffer rasterized from
/// game-submitted occluder volumes. Backend-agnostic (no GPU work).
///
/// Frame protocol:
///   1. BeginFrame(...)              once per frame after camera update
///      (the engine calls this automatically from the main camera)
///   2. SubmitOccluderBox/Triangles  any number of world-space occluders
///   3. FinalizeOccluders()          sort by distance, rasterize budget
///   4. IsVisible(aabb)              frustum + occlusion query
/// Steps 2-3 are optional; IsVisible degrades to frustum-only.
///
/// Everything here is static and lives for the process. The main camera
/// calls BeginFrame() for you during its update, so a game that only wants
/// frustum culling can call IsVisible() and stop reading here.
///
/// Occlusion culling is the part you opt into. Submit occluder volumes
/// every frame, call FinalizeOccluders() once, then test your objects.
/// Occluders must be fully solid volumes: a box submitted over a cave or
/// an open doorway will hide geometry that should be visible. The bounds
/// you test with should be tight around the real vertex extent, since
/// bounds that overshoot into empty space pass the depth test and cull
/// nothing while still costing you the test.
///
/// The occlusion pass adapts. When a rasterized frame culls nothing, it
/// stops rasterizing and probes again every `probeInterval` frames, with
/// queries falling back to frustum-only in between. GetStats() reports
/// what the last pass actually did.
///
/// @code{.cpp}
/// // Optional tuning, once at startup
/// Sleak::CullingSystem::SetOcclusionCullingEnabled(true);
/// Sleak::CullingSystem::SetOcclusionBufferSize(256, 144);
/// Sleak::CullingSystem::SetMaxOccluders(192);
/// Sleak::CullingSystem::SetAdaptiveOcclusion(true, 20);
///
/// // Every frame, after the camera has updated
/// for (const auto& chunk : loadedChunks) {
///     for (const auto& solid : chunk.solidVolumes) {
///         Sleak::CullingSystem::SubmitOccluderBox(solid);
///     }
/// }
/// Sleak::CullingSystem::FinalizeOccluders();
///
/// for (auto& chunk : loadedChunks) {
///     chunk.visible = Sleak::CullingSystem::IsVisible(chunk.bounds);
/// }
///
/// const auto& stats = Sleak::CullingSystem::GetStats();
/// SLEAK_LOG("culled {} by frustum, {} by occlusion",
///           stats.frustumCulled, stats.occlusionCulled);
/// @endcode
///
/// @see ViewFrustum, Camera, Math::AABB
/// @ingroup culling
class ENGINE_API CullingSystem {
public:
    /// Per-frame counters for the last completed culling pass.
    struct Stats {
        uint32_t occludersSubmitted = 0;
        uint32_t occludersRasterized = 0;
        uint32_t tested = 0;
        uint32_t frustumCulled = 0;
        uint32_t occlusionCulled = 0;
        bool occlusionSkipped = false;  // adaptive pass idle this frame
        float rasterizeMs = 0.0f;
    };

    static void SetFrustumCullingEnabled(bool enabled);
    static void SetOcclusionCullingEnabled(bool enabled);
    static bool IsFrustumCullingEnabled();
    static bool IsOcclusionCullingEnabled();

    /// Occlusion depth buffer resolution (default 256x144).
    static void SetOcclusionBufferSize(uint32_t width, uint32_t height);
    /// Max occluders rasterized per frame after the distance sort
    /// (default 192).
    static void SetMaxOccluders(uint32_t count);

    /// Adaptive occlusion (default on, interval 20): when a rasterized
    /// frame culls nothing, skip rasterization for `probeInterval` frames
    /// and probe again. Queries degrade to frustum-only while skipping.
    static void SetAdaptiveOcclusion(bool enabled, uint32_t probeInterval);

    /// viewProj uses the engine row-vector convention: clip = point * VP,
    /// depth range [0, w]. cameraPos is world-space.
    static void BeginFrame(const ViewFrustum& frustum,
                           const Math::Matrix4& viewProj,
                           const Math::Vector3D& cameraPos);

    /// World-space occluders. Boxes must be fully solid volumes.
    static void SubmitOccluderBox(const Math::AABB& box);
    /// World-space triangle occluder; same fully-solid-volume requirement as SubmitOccluderBox.
    static void SubmitOccluderTriangles(const Math::Vector3D* vertices,
                                        uint32_t vertexCount,
                                        const uint32_t* indices,
                                        uint32_t indexCount);
    /// Sorts submitted occluders by distance and rasterizes them into the depth buffer up to the max-occluder budget.
    static void FinalizeOccluders();

    /// Frustum test, then conservative depth test against the occlusion
    /// buffer. Never falsely culls a visible box (given valid occluders).
    static bool IsVisible(const Math::AABB& box);
    /// Frustum-only visibility test, skipping the occlusion buffer entirely.
    static bool IsVisibleFrustumOnly(const Math::AABB& box);

    static const Stats& GetStats();

    /// Debug: row-major width*height floats, NDC depth (0 near, 1 far).
    /// Returns nullptr if occlusion has never rasterized.
    static const float* GetDepthBuffer(uint32_t& width, uint32_t& height);

    static void Shutdown();
};

}  // namespace Sleak

#endif  // _CULLING_SYSTEM_HPP_
