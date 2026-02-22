#ifndef _VIEW_FRUSTUM_HPP_
#define _VIEW_FRUSTUM_HPP_

#include <Math/Matrix.hpp>
#include <Math/Vector.hpp>
#include <cmath>

namespace Sleak {

struct Plane {
    float a, b, c, d;

    float DistanceToPoint(const Math::Vector3D& p) const {
        return a * p.GetX() + b * p.GetY() + c * p.GetZ() + d;
    }

    void Normalize() {
        float len = std::sqrt(a * a + b * b + c * c);
        if (len > 0.0f) {
            float inv = 1.0f / len;
            a *= inv;
            b *= inv;
            c *= inv;
            d *= inv;
        }
    }
};

class ViewFrustum {
public:
    enum { Left = 0, Right, Bottom, Top, Near, Far, COUNT };
    Plane planes[COUNT];

    // Extract frustum planes from VP matrix (row-vector convention: clip = point * VP).
    // Uses column-based Gribb-Hartmann extraction.
    void ExtractFromVP(const Math::Matrix4& m) {
        // Left: col0 + col3  (clip.x >= -clip.w)
        planes[Left].a = m(0, 0) + m(0, 3);
        planes[Left].b = m(1, 0) + m(1, 3);
        planes[Left].c = m(2, 0) + m(2, 3);
        planes[Left].d = m(3, 0) + m(3, 3);

        // Right: col3 - col0  (clip.x <= clip.w)
        planes[Right].a = m(0, 3) - m(0, 0);
        planes[Right].b = m(1, 3) - m(1, 0);
        planes[Right].c = m(2, 3) - m(2, 0);
        planes[Right].d = m(3, 3) - m(3, 0);

        // Bottom: col1 + col3  (clip.y >= -clip.w)
        planes[Bottom].a = m(0, 1) + m(0, 3);
        planes[Bottom].b = m(1, 1) + m(1, 3);
        planes[Bottom].c = m(2, 1) + m(2, 3);
        planes[Bottom].d = m(3, 1) + m(3, 3);

        // Top: col3 - col1  (clip.y <= clip.w)
        planes[Top].a = m(0, 3) - m(0, 1);
        planes[Top].b = m(1, 3) - m(1, 1);
        planes[Top].c = m(2, 3) - m(2, 1);
        planes[Top].d = m(3, 3) - m(3, 1);

        // Near: col2  (clip.z >= 0, depth range [0, w])
        planes[Near].a = m(0, 2);
        planes[Near].b = m(1, 2);
        planes[Near].c = m(2, 2);
        planes[Near].d = m(3, 2);

        // Far: col3 - col2  (clip.z <= clip.w)
        planes[Far].a = m(0, 3) - m(0, 2);
        planes[Far].b = m(1, 3) - m(1, 2);
        planes[Far].c = m(2, 3) - m(2, 2);
        planes[Far].d = m(3, 3) - m(3, 2);

        for (int i = 0; i < COUNT; ++i)
            planes[i].Normalize();
    }

    // Test AABB against frustum using the positive-vertex method.
    // For each plane, find the AABB corner most along the plane normal;
    // if that corner is behind the plane, the AABB is fully outside.
    bool IsAABBVisible(const Math::Vector3D& min, const Math::Vector3D& max) const {
        for (int i = 0; i < COUNT; ++i) {
            // Pick the positive vertex (corner furthest in the plane normal direction)
            float px = (planes[i].a >= 0.0f) ? max.GetX() : min.GetX();
            float py = (planes[i].b >= 0.0f) ? max.GetY() : min.GetY();
            float pz = (planes[i].c >= 0.0f) ? max.GetZ() : min.GetZ();

            if (planes[i].a * px + planes[i].b * py + planes[i].c * pz + planes[i].d < 0.0f)
                return false;
        }
        return true;
    }
};

} // namespace Sleak

#endif
