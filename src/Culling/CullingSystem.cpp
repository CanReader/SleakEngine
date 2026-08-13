#include <Culling/CullingSystem.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace Sleak {
namespace {

constexpr float kWEps = 1e-4f;
constexpr float kZBias = 1e-4f;

/// Clip-space vertex (row-vector convention: clip = point * VP).
struct ClipVert {
    float x, y, z, w;
};

/// One submitted occluder queued for FinalizeOccluders, either a box or a range into triVerts.
struct OccluderEntry {
    bool isBox = true;
    Math::AABB bounds{};
    uint32_t triStart = 0;
    uint32_t triCount = 0;
    float distSq = 0.0f;
};

/// All per-frame culling state: settings, the software depth buffer, and pending occluders.
struct CullState {
    bool frustumEnabled = true;
    bool occlusionEnabled = true;
    uint32_t width = 256;
    uint32_t height = 144;
    uint32_t maxOccluders = 192;

    std::vector<float> depth;
    std::vector<OccluderEntry> occluders;
    std::vector<Math::Vector3D> triVerts;

    ViewFrustum frustum{};
    Math::Matrix4 viewProj{};
    Math::Vector3D cameraPos{};

    bool hasFrame = false;
    bool rasterizedThisFrame = false;
    bool everRasterized = false;

    bool adaptiveEnabled = true;
    uint32_t probeInterval = 20;
    uint32_t skipFrames = 0;

    CullingSystem::Stats stats{};
};

/// Process-wide singleton culling state (the class is all static methods).
CullState& State() {
    static CullState s;
    return s;
}

/// World point -> clip space.
inline ClipVert ToClip(const Math::Vector3D& p, const Math::Matrix4& m) {
    float x = p.GetX(), y = p.GetY(), z = p.GetZ();
    ClipVert c;
    c.x = x * m(0, 0) + y * m(1, 0) + z * m(2, 0) + m(3, 0);
    c.y = x * m(0, 1) + y * m(1, 1) + z * m(2, 1) + m(3, 1);
    c.z = x * m(0, 2) + y * m(1, 2) + z * m(2, 2) + m(3, 2);
    c.w = x * m(0, 3) + y * m(1, 3) + z * m(2, 3) + m(3, 3);
    return c;
}

/// Clip space -> buffer pixels + NDC z (0 near, 1 far).
inline void ToScreen(const ClipVert& c, float w, float h, float& sx,
                     float& sy, float& sz) {
    float inv = 1.0f / c.w;
    float ndcx = c.x * inv;
    float ndcy = c.y * inv;
    sz = c.z * inv;
    sx = (ndcx * 0.5f + 0.5f) * w;
    sy = (0.5f - ndcy * 0.5f) * h;
}

/// Sutherland-Hodgman clip against near plane (w >= kWEps).
int ClipNear(const ClipVert* in, int n, ClipVert* out) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const ClipVert& A = in[i];
        const ClipVert& B = in[(i + 1) % n];
        bool inA = A.w >= kWEps;
        bool inB = B.w >= kWEps;
        if (inA) out[m++] = A;
        if (inA != inB) {
            float t = (kWEps - A.w) / (B.w - A.w);
            ClipVert I;
            I.x = A.x + t * (B.x - A.x);
            I.y = A.y + t * (B.y - A.y);
            I.z = A.z + t * (B.z - A.z);
            I.w = A.w + t * (B.w - A.w);
            out[m++] = I;
        }
    }
    return m;
}

/// Edge-function rasterizer with incremental row stepping, min-depth write.
void RasterScreenTri(float x0, float y0, float z0, float x1, float y1,
                     float z1, float x2, float y2, float z2) {
    CullState& s = State();
    int W = static_cast<int>(s.width);
    int H = static_cast<int>(s.height);

    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (std::fabs(area) < 1e-4f) return;
    float invArea = 1.0f / area;

    float fminX = std::min(x0, std::min(x1, x2));
    float fmaxX = std::max(x0, std::max(x1, x2));
    float fminY = std::min(y0, std::min(y1, y2));
    float fmaxY = std::max(y0, std::max(y1, y2));

    int minX = static_cast<int>(std::floor(fminX));
    int maxX = static_cast<int>(std::ceil(fmaxX));
    int minY = static_cast<int>(std::floor(fminY));
    int maxY = static_cast<int>(std::ceil(fmaxY));
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > W - 1) maxX = W - 1;
    if (maxY > H - 1) maxY = H - 1;
    if (minX > maxX || minY > maxY) return;

    float px0 = minX + 0.5f;
    float py0 = minY + 0.5f;
    float w0Row = (x2 - x1) * (py0 - y1) - (y2 - y1) * (px0 - x1);
    float w1Row = (x0 - x2) * (py0 - y2) - (y0 - y2) * (px0 - x2);
    float w2Row = (x1 - x0) * (py0 - y0) - (y1 - y0) * (px0 - x0);

    float A0 = y1 - y2, A1 = y2 - y0, A2 = y0 - y1;
    float B0 = x2 - x1, B1 = x0 - x2, B2 = x1 - x0;
    bool positive = area > 0.0f;

    for (int y = minY; y <= maxY; ++y) {
        float w0 = w0Row, w1 = w1Row, w2 = w2Row;
        int row = y * W;
        for (int x = minX; x <= maxX; ++x) {
            bool inside = positive ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                                   : (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
            if (inside) {
                float z = (w0 * z0 + w1 * z1 + w2 * z2) * invArea;
                if (z < 0.0f) z = 0.0f;
                if (z > 1.0f) z = 1.0f;
                int idx = row + x;
                if (z < s.depth[idx]) s.depth[idx] = z;
            }
            w0 += A0;
            w1 += A1;
            w2 += A2;
        }
        w0Row += B0;
        w1Row += B1;
        w2Row += B2;
    }
}

/// Near-clip a clip-space triangle, fan-triangulate, rasterize.
void RasterClipTri(const ClipVert& a, const ClipVert& b, const ClipVert& c) {
    ClipVert in[3] = {a, b, c};
    ClipVert out[8];
    int n = ClipNear(in, 3, out);
    if (n < 3) return;

    CullState& s = State();
    float w = static_cast<float>(s.width);
    float h = static_cast<float>(s.height);

    float sx0, sy0, sz0;
    ToScreen(out[0], w, h, sx0, sy0, sz0);
    for (int i = 1; i + 1 < n; ++i) {
        float sx1, sy1, sz1, sx2, sy2, sz2;
        ToScreen(out[i], w, h, sx1, sy1, sz1);
        ToScreen(out[i + 1], w, h, sx2, sy2, sz2);
        RasterScreenTri(sx0, sy0, sz0, sx1, sy1, sz1, sx2, sy2, sz2);
    }
}

/// Projects a world-space triangle and rasterizes it into the depth buffer.
void RasterWorldTri(const Math::Vector3D& p0, const Math::Vector3D& p1,
                    const Math::Vector3D& p2, const Math::Matrix4& vp) {
    RasterClipTri(ToClip(p0, vp), ToClip(p1, vp), ToClip(p2, vp));
}

/// Rasterize up to 3 camera-facing faces of a solid box.
void RasterBox(const Math::AABB& box, const Math::Matrix4& vp,
               const Math::Vector3D& cam) {
    float mnx = box.min.GetX(), mny = box.min.GetY(), mnz = box.min.GetZ();
    float mxx = box.max.GetX(), mxy = box.max.GetY(), mxz = box.max.GetZ();
    using V = Math::Vector3D;
    V c000(mnx, mny, mnz), c100(mxx, mny, mnz);
    V c010(mnx, mxy, mnz), c110(mxx, mxy, mnz);
    V c001(mnx, mny, mxz), c101(mxx, mny, mxz);
    V c011(mnx, mxy, mxz), c111(mxx, mxy, mxz);

    auto quad = [&](const V& a, const V& b, const V& c, const V& d) {
        RasterWorldTri(a, b, c, vp);
        RasterWorldTri(a, c, d, vp);
    };

    if (cam.GetX() < mnx) quad(c000, c001, c011, c010);
    if (cam.GetX() > mxx) quad(c100, c110, c111, c101);
    if (cam.GetY() < mny) quad(c000, c100, c101, c001);
    if (cam.GetY() > mxy) quad(c010, c011, c111, c110);
    if (cam.GetZ() < mnz) quad(c000, c010, c110, c100);
    if (cam.GetZ() > mxz) quad(c001, c101, c111, c011);
}

/// Conservative depth test: true when the box could be in front of the
/// rasterized occluders anywhere in its projected rect.
bool DepthRectVisible(const Math::AABB& box) {
    CullState& s = State();
    float w = static_cast<float>(s.width);
    float h = static_cast<float>(s.height);
    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
    float boxMinZ = 1e30f;
    for (int i = 0; i < 8; ++i) {
        ClipVert c = ToClip(box.Corner(i), s.viewProj);
        if (c.w <= kWEps) return true;
        float sx, sy, sz;
        ToScreen(c, w, h, sx, sy, sz);
        mnx = std::min(mnx, sx);
        mxx = std::max(mxx, sx);
        mny = std::min(mny, sy);
        mxy = std::max(mxy, sy);
        boxMinZ = std::min(boxMinZ, sz);
    }

    int W = static_cast<int>(s.width);
    int H = static_cast<int>(s.height);
    int minX = static_cast<int>(std::floor(mnx)) - 1;
    int maxX = static_cast<int>(std::ceil(mxx)) + 1;
    int minY = static_cast<int>(std::floor(mny)) - 1;
    int maxY = static_cast<int>(std::ceil(mxy)) + 1;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > W - 1) maxX = W - 1;
    if (maxY > H - 1) maxY = H - 1;
    if (minX > maxX || minY > maxY) return true;

    float thresh = boxMinZ - kZBias;
    for (int y = minY; y <= maxY; ++y) {
        int row = y * W;
        for (int x = minX; x <= maxX; ++x) {
            if (s.depth[row + x] >= thresh) return true;
        }
    }
    return false;
}

/// True when the box projects entirely in front but spans < 2x2 px.
bool ProjectedTooSmall(const Math::AABB& b, const Math::Matrix4& vp) {
    CullState& s = State();
    float w = static_cast<float>(s.width);
    float h = static_cast<float>(s.height);
    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
    for (int i = 0; i < 8; ++i) {
        ClipVert c = ToClip(b.Corner(i), vp);
        if (c.w <= kWEps) return false;
        float sx, sy, sz;
        ToScreen(c, w, h, sx, sy, sz);
        mnx = std::min(mnx, sx);
        mxx = std::max(mxx, sx);
        mny = std::min(mny, sy);
        mxy = std::max(mxy, sy);
    }
    return (mxx - mnx) < 2.0f || (mxy - mny) < 2.0f;
}

}  // namespace

void CullingSystem::SetFrustumCullingEnabled(bool enabled) {
    State().frustumEnabled = enabled;
}

void CullingSystem::SetOcclusionCullingEnabled(bool enabled) {
    State().occlusionEnabled = enabled;
}

bool CullingSystem::IsFrustumCullingEnabled() {
    return State().frustumEnabled;
}

bool CullingSystem::IsOcclusionCullingEnabled() {
    return State().occlusionEnabled;
}

void CullingSystem::SetOcclusionBufferSize(uint32_t width, uint32_t height) {
    CullState& s = State();
    s.width = width > 0 ? width : 1;
    s.height = height > 0 ? height : 1;
}

void CullingSystem::SetMaxOccluders(uint32_t count) {
    State().maxOccluders = count;
}

void CullingSystem::SetAdaptiveOcclusion(bool enabled,
                                         uint32_t probeInterval) {
    CullState& s = State();
    s.adaptiveEnabled = enabled;
    s.probeInterval = probeInterval > 0 ? probeInterval : 1;
    if (!enabled) s.skipFrames = 0;
}

void CullingSystem::BeginFrame(const ViewFrustum& frustum,
                               const Math::Matrix4& viewProj,
                               const Math::Vector3D& cameraPos) {
    CullState& s = State();
    if (s.width == 0) s.width = 1;
    if (s.height == 0) s.height = 1;

    // Adaptive: a rasterized frame that culled nothing idles the pass.
    if (s.adaptiveEnabled && s.rasterizedThisFrame &&
        s.stats.occlusionCulled == 0)
        s.skipFrames = s.probeInterval;

    size_t need = static_cast<size_t>(s.width) * s.height;
    if (s.depth.size() != need) s.depth.assign(need, 1.0f);

    s.occluders.clear();
    s.triVerts.clear();
    s.frustum = frustum;
    s.viewProj = viewProj;
    s.cameraPos = cameraPos;
    s.hasFrame = true;
    s.rasterizedThisFrame = false;
    s.stats = Stats{};
}

void CullingSystem::SubmitOccluderBox(const Math::AABB& box) {
    CullState& s = State();
    if (!s.hasFrame || !s.occlusionEnabled) return;
    if (!box.IsValid()) return;
    if (!s.frustum.IsAABBVisible(box.min, box.max)) return;

    OccluderEntry e;
    e.isBox = true;
    e.bounds = box;
    e.distSq = box.DistanceSq(s.cameraPos);
    s.occluders.push_back(e);
    ++s.stats.occludersSubmitted;
}

void CullingSystem::SubmitOccluderTriangles(const Math::Vector3D* vertices,
                                            uint32_t vertexCount,
                                            const uint32_t* indices,
                                            uint32_t indexCount) {
    CullState& s = State();
    if (!s.hasFrame || !s.occlusionEnabled) return;
    if (!vertices || vertexCount == 0) return;
    if (!indices || indexCount < 3) return;
    indexCount -= indexCount % 3;

    Math::AABB bounds(vertices[0], vertices[0]);
    for (uint32_t i = 1; i < vertexCount; ++i)
        bounds.Merge(Math::AABB(vertices[i], vertices[i]));
    if (!s.frustum.IsAABBVisible(bounds.min, bounds.max)) return;

    uint32_t start = static_cast<uint32_t>(s.triVerts.size());
    for (uint32_t i = 0; i < indexCount; ++i) {
        uint32_t idx = indices[i];
        if (idx >= vertexCount) {
            s.triVerts.resize(start);
            return;
        }
        s.triVerts.push_back(vertices[idx]);
    }

    OccluderEntry e;
    e.isBox = false;
    e.bounds = bounds;
    e.triStart = start;
    e.triCount = indexCount;
    e.distSq = bounds.DistanceSq(s.cameraPos);
    s.occluders.push_back(e);
    ++s.stats.occludersSubmitted;
}

void CullingSystem::FinalizeOccluders() {
    CullState& s = State();
    if (!s.hasFrame) return;

    if (s.skipFrames > 0) {
        --s.skipFrames;
        s.occluders.clear();
        s.triVerts.clear();
        s.rasterizedThisFrame = false;
        s.stats.occlusionSkipped = true;
        return;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    std::fill(s.depth.begin(), s.depth.end(), 1.0f);

    std::sort(s.occluders.begin(), s.occluders.end(),
              [](const OccluderEntry& a, const OccluderEntry& b) {
                  return a.distSq < b.distSq;
              });

    uint32_t rasterized = 0;
    for (const auto& e : s.occluders) {
        if (rasterized >= s.maxOccluders) break;
        if (ProjectedTooSmall(e.bounds, s.viewProj)) continue;
        // Occluder fusion: skip occluders fully behind rasterized ones.
        if (rasterized > 0 && !DepthRectVisible(e.bounds)) continue;
        if (e.isBox) {
            RasterBox(e.bounds, s.viewProj, s.cameraPos);
        } else {
            for (uint32_t i = 0; i + 2 < e.triCount; i += 3) {
                RasterWorldTri(s.triVerts[e.triStart + i],
                               s.triVerts[e.triStart + i + 1],
                               s.triVerts[e.triStart + i + 2], s.viewProj);
            }
        }
        ++rasterized;
    }

    s.stats.occludersRasterized = rasterized;
    s.rasterizedThisFrame = rasterized > 0;
    if (rasterized > 0) s.everRasterized = true;

    auto t1 = std::chrono::high_resolution_clock::now();
    s.stats.rasterizeMs =
        std::chrono::duration<float, std::milli>(t1 - t0).count();
}

bool CullingSystem::IsVisible(const Math::AABB& box) {
    CullState& s = State();
    ++s.stats.tested;
    if (!s.hasFrame) return true;

    if (s.frustumEnabled) {
        if (!s.frustum.IsAABBVisible(box.min, box.max)) {
            ++s.stats.frustumCulled;
            return false;
        }
    }

    if (!s.occlusionEnabled || !s.rasterizedThisFrame) return true;

    if (DepthRectVisible(box)) return true;

    ++s.stats.occlusionCulled;
    return false;
}

bool CullingSystem::IsVisibleFrustumOnly(const Math::AABB& box) {
    CullState& s = State();
    ++s.stats.tested;
    if (!s.hasFrame) return true;
    if (s.frustumEnabled) {
        if (!s.frustum.IsAABBVisible(box.min, box.max)) {
            ++s.stats.frustumCulled;
            return false;
        }
    }
    return true;
}

const CullingSystem::Stats& CullingSystem::GetStats() {
    return State().stats;
}

const float* CullingSystem::GetDepthBuffer(uint32_t& width, uint32_t& height) {
    CullState& s = State();
    if (!s.everRasterized || s.depth.empty()) {
        width = 0;
        height = 0;
        return nullptr;
    }
    width = s.width;
    height = s.height;
    return s.depth.data();
}

void CullingSystem::Shutdown() {
    CullState& s = State();
    s.depth.clear();
    s.depth.shrink_to_fit();
    s.occluders.clear();
    s.occluders.shrink_to_fit();
    s.triVerts.clear();
    s.triVerts.shrink_to_fit();
    s.hasFrame = false;
    s.rasterizedThisFrame = false;
    s.everRasterized = false;
    s.stats = Stats{};
}

}  // namespace Sleak
