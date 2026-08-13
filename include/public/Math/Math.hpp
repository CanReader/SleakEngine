#ifndef _MATH_H_
#define _MATH_H_

#include <cmath>
#include <Math/Vector.hpp>

#define PI 3.14159
#define D2R PI / 180.0

namespace Sleak {
namespace Math {

template <typename T>
constexpr const T& Clamp(const T& value, T min, T max) {
    return (value < min) ? min : (value > max) ? max : value;
}

template <typename T>
constexpr const T& Clamp(const T& value, Vector2D range) {
    return (value < range.GetX()) ? range.GetX() : (value > range.GetY()) ? range.GetY() : value;
}

template <typename T>
constexpr const T& Min(const T& value1, const T& value2) {
    return (value1 < value2) ? value1 : value2;
}

template <typename T>
constexpr const T& Max(const T& value1, const T& value2) {
    return (value1 > value2) ? value1 : value2;
}

template <typename T>
constexpr T Lerp(const T& start, const T& end, float t) {
    return start + (end - start) * Clamp(t, 0.0f, 1.0f);
}

}  // namespace Math
}  // namespace Sleak

#endif  // _MATH_H_