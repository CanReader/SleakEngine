#ifndef _COLLISION_DETECTION_HPP_
#define _COLLISION_DETECTION_HPP_

#include <Physics/Colliders.hpp>

namespace Sleak {
namespace Physics {

    /// Single point of contact between two colliding shapes.
    struct ContactPoint {
        Vector3D point;
        Vector3D normal;
        float penetration = 0.0f;
    };

    /// Result of a narrow-phase test: whether a collision occurred and its contact data.
    struct CollisionManifold {
        bool hasCollision = false;
        ContactPoint contact;
    };

    /// Narrow-phase collision tests; the resulting normal always points from A to B.
    CollisionManifold TestAABBvsAABB(const AABB& a, const AABB& b);
    CollisionManifold TestSphereVsSphere(const BoundingSphere& a, const BoundingSphere& b);
    CollisionManifold TestAABBvsSphere(const AABB& a, const BoundingSphere& b);
    CollisionManifold TestSphereVsCapsule(const BoundingSphere& a, const BoundingCapsule& b);
    CollisionManifold TestAABBvsCapsule(const AABB& a, const BoundingCapsule& b);
    CollisionManifold TestCapsuleVsCapsule(const BoundingCapsule& a, const BoundingCapsule& b);
    CollisionManifold TestSphereVsMesh(const BoundingSphere& a, const TriangleMesh& b);
    CollisionManifold TestAABBvsMesh(const AABB& a, const TriangleMesh& b);

    /// Dispatches to the right narrow-phase test based on the runtime shape held by each variant.
    CollisionManifold TestCollision(const ColliderShape& shapeA, const Vector3D& posA, const Vector3D& scaleA,
                                    const ColliderShape& shapeB, const Vector3D& posB, const Vector3D& scaleB);

    /// Nearest point on segment [a, b] to the given point.
    Vector3D ClosestPointOnSegment(const Vector3D& point, const Vector3D& a, const Vector3D& b);
    /// Nearest points between two line segments, used for capsule-vs-capsule tests.
    void ClosestPointsSegmentSegment(const Vector3D& a1, const Vector3D& a2,
                                     const Vector3D& b1, const Vector3D& b2,
                                     Vector3D& closestA, Vector3D& closestB);
    CollisionManifold TestSphereVsTriangle(const BoundingSphere& sphere,
                                           const Vector3D& v0, const Vector3D& v1, const Vector3D& v2);

} // namespace Physics
} // namespace Sleak

#endif // _COLLISION_DETECTION_HPP_
