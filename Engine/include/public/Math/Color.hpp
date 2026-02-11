#ifndef _COLOR_HPP_
#define _COLOR_HPP_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <Math/Math.hpp>
#include <Math/Vector.hpp>

namespace Sleak {
namespace Math {
class Color {
   public:
    // ==============
    // Static Presets
    // ==============
    static const Color Red;
    static const Color Green;
    static const Color Blue;
    static const Color White;
    static const Color Black;
    static const Color Transparent;

    // ==============
    // Constexpr Core
    // ==============
    constexpr Color() = default;
    constexpr Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept
        : r(r), g(g), b(b), a(a) {}

    // ================
    // Color Operations
    // ================
    [[nodiscard]] constexpr Color clamped() const noexcept {
        return {static_cast<uint8_t>(Clamp<float>(r, 0, 255)),
                static_cast<uint8_t>(Clamp<float>(g, 0, 255)),
                static_cast<uint8_t>(Clamp<float>(b, 0, 255)),
                static_cast<uint8_t>(Clamp<float>(a, 0, 255))};
    }

    [[nodiscard]] constexpr Color withAlpha(uint8_t newAlpha) const noexcept {
        return {r, g, b, newAlpha};
    }

    [[nodiscard]] Color premultiplied() const noexcept;

    // =================
    // Color Transforms
    // =================
    [[nodiscard]] Color linearToSrgb() const noexcept;
    [[nodiscard]] Color srgbToLinear() const noexcept;

    // ==============
    // Factory Methods
    // ==============
    [[nodiscard]] static Color fromHSV(float h, float s, float v,
                                       uint8_t a = 255) noexcept;
    [[nodiscard]] static Color fromHex(uint32_t hex) noexcept;

    // ==============
    // String Representation
    // ==============
    [[nodiscard]] std::string toString() const {
        std::ostringstream ss;
        ss << "Color(" << static_cast<int>(r) << ", " << static_cast<int>(g)
           << ", " << static_cast<int>(b) << ", " << static_cast<int>(a) << ")";
        return ss.str();
    }

    // ==============
    // Operators
    // ==============
    constexpr bool operator==(const Color& other) const noexcept {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }

    constexpr bool operator!=(const Color& other) const noexcept {
        return !(*this == other);
    }

    constexpr Color operator+(const Color& other) const noexcept {
        return Color{static_cast<uint8_t>(Clamp(r + other.r, 0, 255)),
                     static_cast<uint8_t>(Clamp(g + other.g, 0, 255)),
                     static_cast<uint8_t>(Clamp(b + other.b, 0, 255)),
                     static_cast<uint8_t>(Clamp(a + other.a, 0, 255))};
    }

    constexpr Color operator*(float scalar) const noexcept {
        return Color{static_cast<uint8_t>(
                         Clamp(static_cast<int>(r * scalar), 0, 255)),
                     static_cast<uint8_t>(
                         Clamp(static_cast<int>(g * scalar), 0, 255)),
                     static_cast<uint8_t>(
                         Clamp(static_cast<int>(b * scalar), 0, 255)),
                     static_cast<uint8_t>(
                         Clamp(static_cast<int>(a * scalar), 0, 255))};
    }

    uint8_t GetR() const { return r; }
    uint8_t GetG() const { return g; }
    uint8_t GetB() const { return b; }
    uint8_t GetA() const { return a; }

    [[nodiscard]] Vector4D normalize() const noexcept {
        return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f};
    }

   private:
    // =====================
    // Fundamental Components
    // =====================
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{255};
};
}  // namespace Math
}  // namespace Sleak

#endif