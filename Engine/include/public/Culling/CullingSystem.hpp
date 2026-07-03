#ifndef _CULLING_SYSTEM_HPP_
#define _CULLING_SYSTEM_HPP_

#include <Camera/ViewFrustum.hpp>
#include <Core/OSDef.hpp>
#include <Math/AABB.hpp>
#include <Math/Matrix.hpp>
#include <Math/Vector.hpp>
#include <cstdint>

namespace Sleak {

// CPU visibility system: view-frustum culling plus software occlusion
// culling against a low-resolution depth buffer rasterized from
// game-submitted occluder volumes. Backend-agnostic (no GPU work).
//
// Frame protocol:
//   1. BeginFrame(...)              once per frame after camera update
//      (the engine calls this automatically from the main camera)
//   2. SubmitOccluderBox/Triangles  any number of world-space occluders
//   3. FinalizeOccluders()          sort by distance, rasterize budget
//   4. IsVisible(aabb)              frustum + occlusion query
// Steps 2-3 are optional; IsVisible degrades to frustum-only.
class ENGINE_API CullingSystem {
public:
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

    // Occlusion depth buffer resolution (default 256x144).
    static void SetOcclusionBufferSize(uint32_t width, uint32_t height);
    // Max occluders rasterized per frame after the distance sort
    // (default 192).
    static void SetMaxOccluders(uint32_t count);

    // Adaptive occlusion (default on, interval 20): when a rasterized
    // frame culls nothing, skip rasterization for `probeInterval` frames
    // and probe again. Queries degrade to frustum-only while skipping.
    static void SetAdaptiveOcclusion(bool enabled, uint32_t probeInterval);

    // viewProj uses the engine row-vector convention: clip = point * VP,
    // depth range [0, w]. cameraPos is world-space.
    static void BeginFrame(const ViewFrustum& frustum,
                           const Math::Matrix4& viewProj,
                           const Math::Vector3D& cameraPos);

    // World-space occluders. Boxes must be fully solid volumes.
    static void SubmitOccluderBox(const Math::AABB& box);
    static void SubmitOccluderTriangles(const Math::Vector3D* vertices,
                                        uint32_t vertexCount,
                                        const uint32_t* indices,
                                        uint32_t indexCount);
    static void FinalizeOccluders();

    // Frustum test, then conservative depth test against the occlusion
    // buffer. Never falsely culls a visible box (given valid occluders).
    static bool IsVisible(const Math::AABB& box);
    static bool IsVisibleFrustumOnly(const Math::AABB& box);

    static const Stats& GetStats();

    // Debug: row-major width*height floats, NDC depth (0 near, 1 far).
    // Returns nullptr if occlusion has never rasterized.
    static const float* GetDepthBuffer(uint32_t& width, uint32_t& height);

    static void Shutdown();
};

}  // namespace Sleak

#endif  // _CULLING_SYSTEM_HPP_
