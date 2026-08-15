#ifndef _AABB_HPP_
#define _AABB_HPP_

#include <Math/Vector.hpp>

namespace Sleak {
namespace Math {

/// Axis-aligned bounding box (world or local space).
/// @ingroup math
struct AABB {
    Vector3D min{0.0f, 0.0f, 0.0f};
    Vector3D max{0.0f, 0.0f, 0.0f};

    AABB() = default;
    AABB(const Vector3D& mn, const Vector3D& mx) : min(mn), max(mx) {}

    /// True if min is componentwise <= max.
    bool IsValid() const {
        return min.GetX() <= max.GetX() && min.GetY() <= max.GetY() &&
               min.GetZ() <= max.GetZ();
    }

    Vector3D Center() const { return (min + max) * 0.5f; }

    Vector3D Extents() const { return (max - min) * 0.5f; }

    // Corner i in [0,8): bit0 -> x, bit1 -> y, bit2 -> z.
    Vector3D Corner(int i) const {
        return Vector3D((i & 1) ? max.GetX() : min.GetX(),
                        (i & 2) ? max.GetY() : min.GetY(),
                        (i & 4) ? max.GetZ() : min.GetZ());
    }

    /// Grows this box to also cover o.
    void Merge(const AABB& o) {
        if (o.min.GetX() < min.GetX()) min.SetX(o.min.GetX());
        if (o.min.GetY() < min.GetY()) min.SetY(o.min.GetY());
        if (o.min.GetZ() < min.GetZ()) min.SetZ(o.min.GetZ());
        if (o.max.GetX() > max.GetX()) max.SetX(o.max.GetX());
        if (o.max.GetY() > max.GetY()) max.SetY(o.max.GetY());
        if (o.max.GetZ() > max.GetZ()) max.SetZ(o.max.GetZ());
    }

    /// Pushes min/max outward by amount on every axis.
    void Expand(float amount) {
        min.Add(-amount, -amount, -amount);
        max.Add(amount, amount, amount);
    }

    /// True if p lies within (or on the boundary of) the box.
    bool Contains(const Vector3D& p) const {
        return p.GetX() >= min.GetX() && p.GetX() <= max.GetX() &&
               p.GetY() >= min.GetY() && p.GetY() <= max.GetY() &&
               p.GetZ() >= min.GetZ() && p.GetZ() <= max.GetZ();
    }

    // Squared distance from a point to this box (0 if inside).
    float DistanceSq(const Vector3D& p) const {
        float d = 0.0f;
        float v;
        v = p.GetX();
        if (v < min.GetX()) d += (min.GetX() - v) * (min.GetX() - v);
        else if (v > max.GetX()) d += (v - max.GetX()) * (v - max.GetX());
        v = p.GetY();
        if (v < min.GetY()) d += (min.GetY() - v) * (min.GetY() - v);
        else if (v > max.GetY()) d += (v - max.GetY()) * (v - max.GetY());
        v = p.GetZ();
        if (v < min.GetZ()) d += (min.GetZ() - v) * (min.GetZ() - v);
        else if (v > max.GetZ()) d += (v - max.GetZ()) * (v - max.GetZ());
        return d;
    }
};

}  // namespace Math
}  // namespace Sleak

#endif  // _AABB_HPP_
