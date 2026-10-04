#include <Physics/CollisionDetection.hpp>
#include <cmath>
#include <algorithm>
#include <limits>

namespace Sleak {
namespace Physics {

Vector3D ClosestPointOnSegment(const Vector3D& point, const Vector3D& a, const Vector3D& b) {
    Vector3D ab = b - a;
    float t = (point - a).Dot(ab);
    float denom = ab.Dot(ab);
    if (denom < 1e-8f) return a;
    t = std::clamp(t / denom, 0.0f, 1.0f);
    return a + ab * t;
}

void ClosestPointsSegmentSegment(const Vector3D& a1, const Vector3D& a2,
                                 const Vector3D& b1, const Vector3D& b2,
                                 Vector3D& closestA, Vector3D& closestB) {
    Vector3D d1 = a2 - a1;
    Vector3D d2 = b2 - b1;
    Vector3D r = a1 - b1;

    float a = d1.Dot(d1);
    float e = d2.Dot(d2);
    float f = d2.Dot(r);

    float s, t;

    if (a < 1e-8f && e < 1e-8f) {
        closestA = a1;
        closestB = b1;
        return;
    }

    if (a < 1e-8f) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        float c = d1.Dot(r);
        if (e < 1e-8f) {
            t = 0.0f;
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            float b = d1.Dot(d2);
            float denom = a * e - b * b;

            if (std::abs(denom) > 1e-8f) {
                s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
            } else {
                s = 0.0f;
            }

            t = (b * s + f) / e;

            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }

    closestA = a1 + d1 * s;
    closestB = b1 + d2 * t;
}

/// Clamps point into the box, giving the nearest surface or interior point.
static Vector3D ClosestPointOnAABB(const AABB& aabb, const Vector3D& point) {
    return Vector3D(
        std::clamp(point.GetX(), aabb.min.GetX(), aabb.max.GetX()),
        std::clamp(point.GetY(), aabb.min.GetY(), aabb.max.GetY()),
        std::clamp(point.GetZ(), aabb.min.GetZ(), aabb.max.GetZ())
    );
}

CollisionManifold TestAABBvsAABB(const AABB& a, const AABB& b) {
    CollisionManifold result;

    if (!a.Overlaps(b)) return result;

    // Find minimum overlap axis
    float overlapX1 = a.max.GetX() - b.min.GetX();
    float overlapX2 = b.max.GetX() - a.min.GetX();
    float overlapY1 = a.max.GetY() - b.min.GetY();
    float overlapY2 = b.max.GetY() - a.min.GetY();
    float overlapZ1 = a.max.GetZ() - b.min.GetZ();
    float overlapZ2 = b.max.GetZ() - a.min.GetZ();

    float minOverlap = overlapX1;
    Vector3D normal(1, 0, 0);

    if (overlapX2 < minOverlap) { minOverlap = overlapX2; normal = Vector3D(-1, 0, 0); }
    if (overlapY1 < minOverlap) { minOverlap = overlapY1; normal = Vector3D(0, 1, 0); }
    if (overlapY2 < minOverlap) { minOverlap = overlapY2; normal = Vector3D(0, -1, 0); }
    if (overlapZ1 < minOverlap) { minOverlap = overlapZ1; normal = Vector3D(0, 0, 1); }
    if (overlapZ2 < minOverlap) { minOverlap = overlapZ2; normal = Vector3D(0, 0, -1); }

    result.hasCollision = true;
    result.contact.normal = normal;
    result.contact.penetration = minOverlap;
    result.contact.point = (a.GetCenter() + b.GetCenter()) * 0.5f;
    return result;
}

CollisionManifold TestSphereVsSphere(const BoundingSphere& a, const BoundingSphere& b) {
    CollisionManifold result;

    Vector3D diff = b.center - a.center;
    float dist = diff.Magnitude();
    float sumR = a.radius + b.radius;

    if (dist >= sumR) return result;

    result.hasCollision = true;
    if (dist > 1e-8f) {
        result.contact.normal = diff * (1.0f / dist);
    } else {
        result.contact.normal = Vector3D(0, 1, 0);
    }
    result.contact.penetration = sumR - dist;
    result.contact.point = a.center + result.contact.normal * a.radius;
    return result;
}

CollisionManifold TestAABBvsSphere(const AABB& a, const BoundingSphere& b) {
    CollisionManifold result;

    Vector3D closest = ClosestPointOnAABB(a, b.center);
    Vector3D diff = b.center - closest;
    float distSq = diff.Dot(diff);

    if (distSq >= b.radius * b.radius) return result;

    float dist = std::sqrt(distSq);
    result.hasCollision = true;
    if (dist > 1e-8f) {
        result.contact.normal = diff * (1.0f / dist);
    } else {
        result.contact.normal = Vector3D(0, 1, 0);
    }
    result.contact.penetration = b.radius - dist;
    result.contact.point = closest;
    return result;
}

CollisionManifold TestSphereVsCapsule(const BoundingSphere& a, const BoundingCapsule& b) {
    Vector3D capA = b.GetPointA();
    Vector3D capB = b.GetPointB();

    Vector3D closest = ClosestPointOnSegment(a.center, capA, capB);

    BoundingSphere capSphere(closest, b.radius);
    return TestSphereVsSphere(a, capSphere);
}

CollisionManifold TestAABBvsCapsule(const AABB& a, const BoundingCapsule& b) {
    Vector3D capA = b.GetPointA();
    Vector3D capB = b.GetPointB();

    // Find closest point on capsule segment to AABB center, then do AABB vs Sphere
    Vector3D aabbCenter = a.GetCenter();
    Vector3D closestOnSeg = ClosestPointOnSegment(aabbCenter, capA, capB);

    BoundingSphere testSphere(closestOnSeg, b.radius);
    return TestAABBvsSphere(a, testSphere);
}

CollisionManifold TestCapsuleVsCapsule(const BoundingCapsule& a, const BoundingCapsule& b) {
    Vector3D a1 = a.GetPointA(), a2 = a.GetPointB();
    Vector3D b1 = b.GetPointA(), b2 = b.GetPointB();

    Vector3D closestA, closestB;
    ClosestPointsSegmentSegment(a1, a2, b1, b2, closestA, closestB);

    BoundingSphere sA(closestA, a.radius);
    BoundingSphere sB(closestB, b.radius);
    return TestSphereVsSphere(sA, sB);
}

CollisionManifold TestSphereVsTriangle(const BoundingSphere& sphere,
                                       const Vector3D& v0, const Vector3D& v1, const Vector3D& v2) {
    CollisionManifold result;

    // Project sphere center onto triangle plane
    Vector3D edge0 = v1 - v0;
    Vector3D edge1 = v2 - v0;
    Vector3D n = edge0.Cross(edge1);
    float nLen = n.Magnitude();
    if (nLen < 1e-8f) return result;
    n = n * (1.0f / nLen);

    float dist = (sphere.center - v0).Dot(n);
    if (std::abs(dist) > sphere.radius) return result;

    // Closest point on triangle
    Vector3D proj = sphere.center - n * dist;

    // Barycentric test - check if projected point is inside triangle
    Vector3D v0p = proj - v0;
    float d00 = edge0.Dot(edge0);
    float d01 = edge0.Dot(edge1);
    float d11 = edge1.Dot(edge1);
    float d20 = v0p.Dot(edge0);
    float d21 = v0p.Dot(edge1);
    float denom = d00 * d11 - d01 * d01;

    if (std::abs(denom) < 1e-8f) return result;

    float bv = (d11 * d20 - d01 * d21) / denom;
    float bw = (d00 * d21 - d01 * d20) / denom;
    float bu = 1.0f - bv - bw;

    Vector3D closestPoint;
    if (bu >= 0 && bv >= 0 && bw >= 0) {
        closestPoint = proj;
    } else {
        // Closest point on edges
        Vector3D c0 = ClosestPointOnSegment(sphere.center, v0, v1);
        Vector3D c1 = ClosestPointOnSegment(sphere.center, v1, v2);
        Vector3D c2 = ClosestPointOnSegment(sphere.center, v2, v0);

        float d0 = (sphere.center - c0).Dot(sphere.center - c0);
        float d1 = (sphere.center - c1).Dot(sphere.center - c1);
        float d2 = (sphere.center - c2).Dot(sphere.center - c2);

        closestPoint = c0;
        float minD = d0;
        if (d1 < minD) { minD = d1; closestPoint = c1; }
        if (d2 < minD) { closestPoint = c2; }
    }

    Vector3D diff = sphere.center - closestPoint;
    float distSq = diff.Dot(diff);
    if (distSq >= sphere.radius * sphere.radius) return result;

    float d = std::sqrt(distSq);
    result.hasCollision = true;
    result.contact.point = closestPoint;
    result.contact.penetration = sphere.radius - d;
    if (d > 1e-8f) {
        result.contact.normal = diff * (1.0f / d);
    } else {
        result.contact.normal = n;
    }
    return result;
}

CollisionManifold TestSphereVsMesh(const BoundingSphere& a, const TriangleMesh& b) {
    CollisionManifold deepest;

    for (size_t i = 0; i + 2 < b.indices.size(); i += 3) {
        const Vector3D& v0 = b.vertices[b.indices[i]];
        const Vector3D& v1 = b.vertices[b.indices[i + 1]];
        const Vector3D& v2 = b.vertices[b.indices[i + 2]];

        CollisionManifold m = TestSphereVsTriangle(a, v0, v1, v2);
        if (m.hasCollision && m.contact.penetration > deepest.contact.penetration) {
            deepest = m;
        }
    }

    return deepest;
}

CollisionManifold TestAABBvsMesh(const AABB& a, const TriangleMesh& b) {
    // Approximate: use sphere enclosing AABB, test against mesh
    BoundingSphere approx = BoundingSphere::FromAABB(a);
    return TestSphereVsMesh(approx, b);
}

static AABB TransformAABB(const AABB& aabb, const Vector3D& pos, const Vector3D& scale) {
    Vector3D sMin = aabb.min * scale + pos;
    Vector3D sMax = aabb.max * scale + pos;
    return AABB(
        Vector3D(std::min(sMin.GetX(), sMax.GetX()),
                 std::min(sMin.GetY(), sMax.GetY()),
                 std::min(sMin.GetZ(), sMax.GetZ())),
        Vector3D(std::max(sMin.GetX(), sMax.GetX()),
                 std::max(sMin.GetY(), sMax.GetY()),
                 std::max(sMin.GetZ(), sMax.GetZ()))
    );
}

static BoundingSphere TransformSphere(const BoundingSphere& s, const Vector3D& pos, const Vector3D& scale) {
    float maxScale = std::max({std::abs(scale.GetX()), std::abs(scale.GetY()), std::abs(scale.GetZ())});
    return BoundingSphere(s.center * scale + pos, s.radius * maxScale);
}

static BoundingCapsule TransformCapsule(const BoundingCapsule& c, const Vector3D& pos, const Vector3D& scale) {
    float maxScale = std::max({std::abs(scale.GetX()), std::abs(scale.GetY()), std::abs(scale.GetZ())});
    float axisScale = 1.0f;
    if (c.axis == 0) axisScale = std::abs(scale.GetX());
    else if (c.axis == 1) axisScale = std::abs(scale.GetY());
    else axisScale = std::abs(scale.GetZ());

    return BoundingCapsule(c.center * scale + pos, c.radius * maxScale, c.halfHeight * axisScale, c.axis);
}

static TriangleMesh TransformMesh(const TriangleMesh& m, const Vector3D& pos, const Vector3D& scale) {
    TriangleMesh result;
    result.indices = m.indices;
    result.vertices.resize(m.vertices.size());
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        result.vertices[i] = m.vertices[i] * scale + pos;
    }
    result.bounds = TransformAABB(m.bounds, pos, scale);
    return result;
}

CollisionManifold TestCollision(const ColliderShape& shapeA, const Vector3D& posA, const Vector3D& scaleA,
                                const ColliderShape& shapeB, const Vector3D& posB, const Vector3D& scaleB) {
    return std::visit([&](const auto& a, const auto& b) -> CollisionManifold {
        using A = std::decay_t<decltype(a)>;
        using B = std::decay_t<decltype(b)>;

        if constexpr (std::is_same_v<A, AABB> && std::is_same_v<B, AABB>) {
            return TestAABBvsAABB(TransformAABB(a, posA, scaleA), TransformAABB(b, posB, scaleB));
        }
        else if constexpr (std::is_same_v<A, BoundingSphere> && std::is_same_v<B, BoundingSphere>) {
            return TestSphereVsSphere(TransformSphere(a, posA, scaleA), TransformSphere(b, posB, scaleB));
        }
        else if constexpr (std::is_same_v<A, AABB> && std::is_same_v<B, BoundingSphere>) {
            return TestAABBvsSphere(TransformAABB(a, posA, scaleA), TransformSphere(b, posB, scaleB));
        }
        else if constexpr (std::is_same_v<A, BoundingSphere> && std::is_same_v<B, AABB>) {
            auto m = TestAABBvsSphere(TransformAABB(b, posB, scaleB), TransformSphere(a, posA, scaleA));
            m.contact.normal = m.contact.normal * -1.0f;
            return m;
        }
        else if constexpr (std::is_same_v<A, BoundingSphere> && std::is_same_v<B, BoundingCapsule>) {
            return TestSphereVsCapsule(TransformSphere(a, posA, scaleA), TransformCapsule(b, posB, scaleB));
        }
        else if constexpr (std::is_same_v<A, BoundingCapsule> && std::is_same_v<B, BoundingSphere>) {
            auto m = TestSphereVsCapsule(TransformSphere(b, posB, scaleB), TransformCapsule(a, posA, scaleA));
            m.contact.normal = m.contact.normal * -1.0f;
            return m;
        }
        else if constexpr (std::is_same_v<A, AABB> && std::is_same_v<B, BoundingCapsule>) {
            return TestAABBvsCapsule(TransformAABB(a, posA, scaleA), TransformCapsule(b, posB, scaleB));
        }
        else if constexpr (std::is_same_v<A, BoundingCapsule> && std::is_same_v<B, AABB>) {
            auto m = TestAABBvsCapsule(TransformAABB(b, posB, scaleB), TransformCapsule(a, posA, scaleA));
            m.contact.normal = m.contact.normal * -1.0f;
            return m;
        }
        else if constexpr (std::is_same_v<A, BoundingCapsule> && std::is_same_v<B, BoundingCapsule>) {
            return TestCapsuleVsCapsule(TransformCapsule(a, posA, scaleA), TransformCapsule(b, posB, scaleB));
        }
        else if constexpr (std::is_same_v<A, BoundingSphere> && std::is_same_v<B, TriangleMesh>) {
            return TestSphereVsMesh(TransformSphere(a, posA, scaleA), TransformMesh(b, posB, scaleB));
        }
        else if constexpr (std::is_same_v<A, TriangleMesh> && std::is_same_v<B, BoundingSphere>) {
            auto m = TestSphereVsMesh(TransformSphere(b, posB, scaleB), TransformMesh(a, posA, scaleA));
            m.contact.normal = m.contact.normal * -1.0f;
            return m;
        }
        else if constexpr (std::is_same_v<A, AABB> && std::is_same_v<B, TriangleMesh>) {
            return TestAABBvsMesh(TransformAABB(a, posA, scaleA), TransformMesh(b, posB, scaleB));
        }
        else if constexpr (std::is_same_v<A, TriangleMesh> && std::is_same_v<B, AABB>) {
            auto m = TestAABBvsMesh(TransformAABB(b, posB, scaleB), TransformMesh(a, posA, scaleA));
            m.contact.normal = m.contact.normal * -1.0f;
            return m;
        }
        else {
            // TriangleMesh vs TriangleMesh or Capsule vs Mesh - not supported yet
            return CollisionManifold{};
        }
    }, shapeA, shapeB);
}

/// Slab test for any ray direction length; reports the entry distance and axis.
static bool RaySlab(const AABB& box, const Vector3D& origin,
                    const Vector3D& dir, float maxDist, float& tEnter,
                    int& enterAxis) {
    float tmin = 0.0f;
    float tmax = maxDist;
    enterAxis = -1;

    const float o[3] = {origin.GetX(), origin.GetY(), origin.GetZ()};
    const float d[3] = {dir.GetX(), dir.GetY(), dir.GetZ()};
    const float lo[3] = {box.min.GetX(), box.min.GetY(), box.min.GetZ()};
    const float hi[3] = {box.max.GetX(), box.max.GetY(), box.max.GetZ()};

    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-12f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
            continue;
        }
        float inv = 1.0f / d[i];
        float t1 = (lo[i] - o[i]) * inv;
        float t2 = (hi[i] - o[i]) * inv;
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > tmin) {
            tmin = t1;
            enterAxis = i;
        }
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }

    tEnter = tmin;
    return true;
}

bool RaycastAABB(const AABB& box, const Vector3D& origin, const Vector3D& dir,
                 float maxDist, float& t, Vector3D& normal) {
    int axis = -1;
    float tEnter = 0.0f;
    if (!RaySlab(box, origin, dir, maxDist, tEnter, axis)) return false;
    if (axis < 0) return false;  // origin inside the box

    const float d[3] = {dir.GetX(), dir.GetY(), dir.GetZ()};
    float n[3] = {0.0f, 0.0f, 0.0f};
    n[axis] = d[axis] > 0.0f ? -1.0f : 1.0f;

    t = tEnter;
    normal = Vector3D(n[0], n[1], n[2]);
    return true;
}

bool RaycastSphere(const BoundingSphere& sphere, const Vector3D& origin,
                   const Vector3D& dir, float maxDist, float& t,
                   Vector3D& normal) {
    Vector3D m = origin - sphere.center;
    float c = m.Dot(m) - sphere.radius * sphere.radius;
    if (c <= 0.0f) return false;  // origin inside

    float b = m.Dot(dir);
    if (b > 0.0f) return false;

    float disc = b * b - c;
    if (disc < 0.0f) return false;

    float hitT = -b - std::sqrt(disc);
    if (hitT < 0.0f) hitT = 0.0f;
    if (hitT > maxDist) return false;

    t = hitT;
    if (sphere.radius > 1e-8f) {
        normal = (origin + dir * hitT - sphere.center) * (1.0f / sphere.radius);
    } else {
        normal = dir * -1.0f;
    }
    return true;
}

bool RaycastCapsule(const BoundingCapsule& capsule, const Vector3D& origin,
                    const Vector3D& dir, float maxDist, float& t,
                    Vector3D& normal) {
    Vector3D pa = capsule.GetPointB();
    Vector3D pb = capsule.GetPointA();
    float r = capsule.radius;

    Vector3D toOrigin = origin - ClosestPointOnSegment(origin, pa, pb);
    if (toOrigin.Dot(toOrigin) <= r * r) return false;  // origin inside

    float best = std::numeric_limits<float>::max();

    Vector3D ba = pb - pa;
    float baba = ba.Dot(ba);
    if (baba > 1e-12f) {
        Vector3D oa = origin - pa;
        float bard = ba.Dot(dir);
        float baoa = ba.Dot(oa);
        float rdoa = dir.Dot(oa);
        float oaoa = oa.Dot(oa);
        float a = baba - bard * bard;
        if (a > 1e-8f * baba) {
            float b = baba * rdoa - baoa * bard;
            float c = baba * oaoa - baoa * baoa - r * r * baba;
            float h = b * b - a * c;
            if (h >= 0.0f) {
                float hitT = (-b - std::sqrt(h)) / a;
                float y = baoa + hitT * bard;
                if (hitT >= 0.0f && y > 0.0f && y < baba) best = hitT;
            }
        }
    }

    float capT = 0.0f;
    Vector3D capN;
    if (RaycastSphere(BoundingSphere(pa, r), origin, dir, maxDist, capT,
                      capN) &&
        capT < best) {
        best = capT;
    }
    if (RaycastSphere(BoundingSphere(pb, r), origin, dir, maxDist, capT,
                      capN) &&
        capT < best) {
        best = capT;
    }

    if (best > maxDist) return false;

    Vector3D hit = origin + dir * best;
    Vector3D out = hit - ClosestPointOnSegment(hit, pa, pb);
    float len = out.Magnitude();
    t = best;
    normal = len > 1e-8f ? out * (1.0f / len) : dir * -1.0f;
    return true;
}

bool RaycastTriangle(const Vector3D& v0, const Vector3D& v1, const Vector3D& v2,
                     const Vector3D& origin, const Vector3D& dir, float maxDist,
                     float& t, Vector3D& normal) {
    Vector3D e1 = v1 - v0;
    Vector3D e2 = v2 - v0;
    Vector3D p = dir.Cross(e2);
    float det = e1.Dot(p);
    if (std::abs(det) < 1e-12f) return false;

    float invDet = 1.0f / det;
    Vector3D s = origin - v0;
    float u = s.Dot(p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    Vector3D q = s.Cross(e1);
    float v = dir.Dot(q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;

    float hitT = e2.Dot(q) * invDet;
    if (hitT < 0.0f || hitT > maxDist) return false;

    Vector3D n = e1.Cross(e2);
    if (n.Dot(dir) > 0.0f) n = n * -1.0f;
    float len = n.Magnitude();
    if (len < 1e-12f) return false;

    t = hitT;
    normal = n * (1.0f / len);
    return true;
}

/// Ray against a mesh, done in the mesh's local space so scale needs no copy.
static bool RaycastMesh(const TriangleMesh& mesh, const Vector3D& pos,
                        const Vector3D& scale, const Vector3D& origin,
                        const Vector3D& dir, float maxDist, float& t,
                        Vector3D& normal) {
    float sx = scale.GetX(), sy = scale.GetY(), sz = scale.GetZ();
    if (std::abs(sx) < 1e-12f || std::abs(sy) < 1e-12f || std::abs(sz) < 1e-12f)
        return false;

    Vector3D rel = origin - pos;
    Vector3D localOrigin(rel.GetX() / sx, rel.GetY() / sy, rel.GetZ() / sz);
    Vector3D localDir(dir.GetX() / sx, dir.GetY() / sy, dir.GetZ() / sz);

    // t is shared between spaces because the ray is mapped affinely
    float boundsT = 0.0f;
    int axis = -1;
    if (!RaySlab(mesh.bounds, localOrigin, localDir, maxDist, boundsT, axis))
        return false;

    bool hit = false;
    float best = maxDist;
    Vector3D bestLocalN;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1],
                 i2 = mesh.indices[i + 2];
        if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() ||
            i2 >= mesh.vertices.size())
            continue;
        float triT = 0.0f;
        Vector3D triN;
        if (RaycastTriangle(mesh.vertices[i0], mesh.vertices[i1],
                            mesh.vertices[i2], localOrigin, localDir, best,
                            triT, triN)) {
            best = triT;
            bestLocalN = triN;
            hit = true;
        }
    }
    if (!hit) return false;

    Vector3D n(bestLocalN.GetX() / sx, bestLocalN.GetY() / sy,
               bestLocalN.GetZ() / sz);
    float len = n.Magnitude();
    if (len < 1e-12f) return false;
    n = n * (1.0f / len);
    if (n.Dot(dir) > 0.0f) n = n * -1.0f;

    t = best;
    normal = n;
    return true;
}

bool RaycastShape(const ColliderShape& shape, const Vector3D& pos,
                  const Vector3D& scale, const Vector3D& origin,
                  const Vector3D& dir, float maxDist, float& t,
                  Vector3D& normal) {
    if (auto* box = std::get_if<AABB>(&shape)) {
        return RaycastAABB(TransformAABB(*box, pos, scale), origin, dir,
                           maxDist, t, normal);
    }
    if (auto* sphere = std::get_if<BoundingSphere>(&shape)) {
        return RaycastSphere(TransformSphere(*sphere, pos, scale), origin, dir,
                             maxDist, t, normal);
    }
    if (auto* capsule = std::get_if<BoundingCapsule>(&shape)) {
        return RaycastCapsule(TransformCapsule(*capsule, pos, scale), origin,
                              dir, maxDist, t, normal);
    }
    if (auto* mesh = std::get_if<TriangleMesh>(&shape)) {
        return RaycastMesh(*mesh, pos, scale, origin, dir, maxDist, t, normal);
    }
    return false;
}

} // namespace Physics
} // namespace Sleak
