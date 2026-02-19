#include <Physics/CollisionDetection.hpp>
#include <cmath>
#include <algorithm>
#include <limits>

namespace Sleak {
namespace Physics {

// --- Utilities ---

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

static Vector3D ClosestPointOnAABB(const AABB& aabb, const Vector3D& point) {
    return Vector3D(
        std::clamp(point.GetX(), aabb.min.GetX(), aabb.max.GetX()),
        std::clamp(point.GetY(), aabb.min.GetY(), aabb.max.GetY()),
        std::clamp(point.GetZ(), aabb.min.GetZ(), aabb.max.GetZ())
    );
}

// --- AABB vs AABB ---

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

// --- Sphere vs Sphere ---

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

// --- AABB vs Sphere ---

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

// --- Sphere vs Capsule ---

CollisionManifold TestSphereVsCapsule(const BoundingSphere& a, const BoundingCapsule& b) {
    Vector3D capA = b.GetPointA();
    Vector3D capB = b.GetPointB();

    Vector3D closest = ClosestPointOnSegment(a.center, capA, capB);

    BoundingSphere capSphere(closest, b.radius);
    return TestSphereVsSphere(a, capSphere);
}

// --- AABB vs Capsule ---

CollisionManifold TestAABBvsCapsule(const AABB& a, const BoundingCapsule& b) {
    Vector3D capA = b.GetPointA();
    Vector3D capB = b.GetPointB();

    // Find closest point on capsule segment to AABB center, then do AABB vs Sphere
    Vector3D aabbCenter = a.GetCenter();
    Vector3D closestOnSeg = ClosestPointOnSegment(aabbCenter, capA, capB);

    BoundingSphere testSphere(closestOnSeg, b.radius);
    return TestAABBvsSphere(a, testSphere);
}

// --- Capsule vs Capsule ---

CollisionManifold TestCapsuleVsCapsule(const BoundingCapsule& a, const BoundingCapsule& b) {
    Vector3D a1 = a.GetPointA(), a2 = a.GetPointB();
    Vector3D b1 = b.GetPointA(), b2 = b.GetPointB();

    Vector3D closestA, closestB;
    ClosestPointsSegmentSegment(a1, a2, b1, b2, closestA, closestB);

    BoundingSphere sA(closestA, a.radius);
    BoundingSphere sB(closestB, b.radius);
    return TestSphereVsSphere(sA, sB);
}

// --- Sphere vs Triangle ---

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

// --- Sphere vs Mesh ---

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

// --- AABB vs Mesh ---

CollisionManifold TestAABBvsMesh(const AABB& a, const TriangleMesh& b) {
    // Approximate: use sphere enclosing AABB, test against mesh
    BoundingSphere approx = BoundingSphere::FromAABB(a);
    return TestSphereVsMesh(approx, b);
}

// --- Transform helpers for dispatch ---

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

// --- Dispatcher ---

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

} // namespace Physics
} // namespace Sleak
