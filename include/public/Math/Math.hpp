#ifndef _MATH_H_
#define _MATH_H_

#include <cmath>
#include <Math/Vector.hpp>

#define PI 3.14159
#define D2R PI / 180.0

namespace Sleak {
namespace Math {

/// Clamps value into [min, max].
/// Returns by value: min and max are parameters, so returning a reference
/// would hand back a dangling one whenever clamping engages.
template <typename T>
constexpr T Clamp(const T& value, T min, T max) {
    return (value < min) ? min : (value > max) ? max : value;
}

/// Clamps value into [range.x, range.y].
template <typename T>
constexpr T Clamp(const T& value, Vector2D range) {
    return (value < range.GetX())   ? static_cast<T>(range.GetX())
           : (value > range.GetY()) ? static_cast<T>(range.GetY())
                                    : value;
}

template <typename T>
constexpr const T& Min(const T& value1, const T& value2) {
    return (value1 < value2) ? value1 : value2;
}

template <typename T>
constexpr const T& Max(const T& value1, const T& value2) {
    return (value1 > value2) ? value1 : value2;
}

/// Linear interpolation from start to end; t is clamped to [0,1].
template <typename T>
constexpr T Lerp(const T& start, const T& end, float t) {
    return start + (end - start) * Clamp(t, 0.0f, 1.0f);
}

}  // namespace Math
}  // namespace Sleak

#endif  // _MATH_H_