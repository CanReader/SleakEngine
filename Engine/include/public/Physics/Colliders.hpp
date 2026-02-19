#ifndef _COLLIDERS_HPP_
#define _COLLIDERS_HPP_

#include <Math/Vector.hpp>
#include <Utility/Container/List.hpp>
#include <variant>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Sleak {
namespace Physics {

    using Math::Vector3D;

    struct AABB {
        Vector3D min;
        Vector3D max;

        AABB() : min(Vector3D(0, 0, 0)), max(Vector3D(0, 0, 0)) {}
        AABB(const Vector3D& min, const Vector3D& max) : min(min), max(max) {}

        Vector3D GetCenter() const {
            return (min + max) * 0.5f;
        }

        Vector3D GetExtents() const {
            return (max - min) * 0.5f;
        }

        float GetSurfaceArea() const {
            Vector3D d = max - min;
            return 2.0f * (d.GetX() * d.GetY() + d.GetY() * d.GetZ() + d.GetZ() * d.GetX());
        }

        bool Contains(const Vector3D& point) const {
            return point.GetX() >= min.GetX() && point.GetX() <= max.GetX() &&
                   point.GetY() >= min.GetY() && point.GetY() <= max.GetY() &&
                   point.GetZ() >= min.GetZ() && point.GetZ() <= max.GetZ();
        }

        bool Overlaps(const AABB& other) const {
            if (max.GetX() < other.min.GetX() || min.GetX() > other.max.GetX()) return false;
            if (max.GetY() < other.min.GetY() || min.GetY() > other.max.GetY()) return false;
            if (max.GetZ() < other.min.GetZ() || min.GetZ() > other.max.GetZ()) return false;
            return true;
        }

        AABB Merge(const AABB& other) const {
            return AABB(
                Vector3D(std::min(min.GetX(), other.min.GetX()),
                         std::min(min.GetY(), other.min.GetY()),
                         std::min(min.GetZ(), other.min.GetZ())),
                Vector3D(std::max(max.GetX(), other.max.GetX()),
                         std::max(max.GetY(), other.max.GetY()),
                         std::max(max.GetZ(), other.max.GetZ()))
            );
        }

        AABB Fatten(float margin) const {
            return AABB(
                Vector3D(min.GetX() - margin, min.GetY() - margin, min.GetZ() - margin),
                Vector3D(max.GetX() + margin, max.GetY() + margin, max.GetZ() + margin)
            );
        }

        static AABB FromVertices(const float* positions, size_t count, size_t stride) {
            if (count == 0) return AABB();

            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float maxY = std::numeric_limits<float>::lowest();
            float maxZ = std::numeric_limits<float>::lowest();

            const char* ptr = reinterpret_cast<const char*>(positions);
            for (size_t i = 0; i < count; ++i) {
                const float* pos = reinterpret_cast<const float*>(ptr + i * stride);
                if (pos[0] < minX) minX = pos[0];
                if (pos[1] < minY) minY = pos[1];
                if (pos[2] < minZ) minZ = pos[2];
                if (pos[0] > maxX) maxX = pos[0];
                if (pos[1] > maxY) maxY = pos[1];
                if (pos[2] > maxZ) maxZ = pos[2];
            }

            return AABB(Vector3D(minX, minY, minZ), Vector3D(maxX, maxY, maxZ));
        }
    };

    struct BoundingSphere {
        Vector3D center;
        float radius;

        BoundingSphere() : center(Vector3D(0, 0, 0)), radius(0.0f) {}
        BoundingSphere(const Vector3D& center, float radius)
            : center(center), radius(radius) {}

        bool Contains(const Vector3D& point) const {
            return (point - center).Magnitude() <= radius;
        }

        bool Overlaps(const BoundingSphere& other) const {
            float dist = (center - other.center).Magnitude();
            return dist <= (radius + other.radius);
        }

        AABB ToAABB() const {
            return AABB(
                Vector3D(center.GetX() - radius, center.GetY() - radius, center.GetZ() - radius),
                Vector3D(center.GetX() + radius, center.GetY() + radius, center.GetZ() + radius)
            );
        }

        static BoundingSphere FromAABB(const AABB& aabb) {
            Vector3D center = aabb.GetCenter();
            float radius = (aabb.max - center).Magnitude();
            return BoundingSphere(center, radius);
        }
    };

    struct BoundingCapsule {
        Vector3D center;
        float radius;
        float halfHeight;
        int axis; // 0=X, 1=Y, 2=Z

        BoundingCapsule()
            : center(Vector3D(0, 0, 0)), radius(0.5f), halfHeight(0.5f), axis(1) {}
        BoundingCapsule(const Vector3D& center, float radius, float halfHeight, int axis = 1)
            : center(center), radius(radius), halfHeight(halfHeight), axis(axis) {}

        Vector3D GetPointA() const {
            Vector3D offset(0, 0, 0);
            if (axis == 0) offset.SetX(halfHeight);
            else if (axis == 1) offset.SetY(halfHeight);
            else offset.SetZ(halfHeight);
            return center + offset;
        }

        Vector3D GetPointB() const {
            Vector3D offset(0, 0, 0);
            if (axis == 0) offset.SetX(-halfHeight);
            else if (axis == 1) offset.SetY(-halfHeight);
            else offset.SetZ(-halfHeight);
            return center + offset;
        }

        AABB ToAABB() const {
            Vector3D a = GetPointA();
            Vector3D b = GetPointB();
            Vector3D minPt(
                std::min(a.GetX(), b.GetX()) - radius,
                std::min(a.GetY(), b.GetY()) - radius,
                std::min(a.GetZ(), b.GetZ()) - radius
            );
            Vector3D maxPt(
                std::max(a.GetX(), b.GetX()) + radius,
                std::max(a.GetY(), b.GetY()) + radius,
                std::max(a.GetZ(), b.GetZ()) + radius
            );
            return AABB(minPt, maxPt);
        }

        static BoundingCapsule FromAABB(const AABB& aabb) {
            Vector3D center = aabb.GetCenter();
            Vector3D extents = aabb.GetExtents();

            // Choose longest axis
            int axis = 1;
            float maxExt = extents.GetY();
            if (extents.GetX() > maxExt) { axis = 0; maxExt = extents.GetX(); }
            if (extents.GetZ() > maxExt) { axis = 2; maxExt = extents.GetZ(); }

            // Radius from the shorter two axes
            float r = 0.0f;
            if (axis == 0) r = std::max(extents.GetY(), extents.GetZ());
            else if (axis == 1) r = std::max(extents.GetX(), extents.GetZ());
            else r = std::max(extents.GetX(), extents.GetY());

            float halfHeight = maxExt - r;
            if (halfHeight < 0.0f) halfHeight = 0.0f;

            return BoundingCapsule(center, r, halfHeight, axis);
        }
    };

    struct TriangleMesh {
        std::vector<Vector3D> vertices;
        std::vector<uint32_t> indices;
        AABB bounds;

        void Build(const float* positions, size_t count, size_t stride,
                   const uint32_t* indexData, size_t indexCount) {
            vertices.resize(count);
            const char* ptr = reinterpret_cast<const char*>(positions);
            for (size_t i = 0; i < count; ++i) {
                const float* pos = reinterpret_cast<const float*>(ptr + i * stride);
                vertices[i] = Vector3D(pos[0], pos[1], pos[2]);
            }
            indices.assign(indexData, indexData + indexCount);
            bounds = AABB::FromVertices(positions, count, stride);
        }
    };

    enum class ColliderType {
        AABB,
        Sphere,
        Capsule,
        Mesh
    };

    using ColliderShape = std::variant<AABB, BoundingSphere, BoundingCapsule, TriangleMesh>;

    // Transform a local collider shape to a world-space AABB for broadphase
    inline AABB GetWorldAABB(const ColliderShape& shape, const Vector3D& worldPos, const Vector3D& worldScale) {
        AABB local;

        if (auto* aabb = std::get_if<AABB>(&shape)) {
            local = *aabb;
        } else if (auto* sphere = std::get_if<BoundingSphere>(&shape)) {
            local = sphere->ToAABB();
        } else if (auto* capsule = std::get_if<BoundingCapsule>(&shape)) {
            local = capsule->ToAABB();
        } else if (auto* mesh = std::get_if<TriangleMesh>(&shape)) {
            local = mesh->bounds;
        }

        // Scale then translate
        Vector3D scaledMin = local.min * worldScale + worldPos;
        Vector3D scaledMax = local.max * worldScale + worldPos;

        // Fix inverted axes from negative scale
        return AABB(
            Vector3D(std::min(scaledMin.GetX(), scaledMax.GetX()),
                     std::min(scaledMin.GetY(), scaledMax.GetY()),
                     std::min(scaledMin.GetZ(), scaledMax.GetZ())),
            Vector3D(std::max(scaledMin.GetX(), scaledMax.GetX()),
                     std::max(scaledMin.GetY(), scaledMax.GetY()),
                     std::max(scaledMin.GetZ(), scaledMax.GetZ()))
        );
    }

} // namespace Physics
} // namespace Sleak

#endif // _COLLIDERS_HPP_
