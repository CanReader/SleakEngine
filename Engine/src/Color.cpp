#include <Math/Color.hpp>

namespace Sleak {
namespace Math {

const Color Color::Red(255, 0, 0);
const Color Color::Green(0, 255, 0);
const Color Color::Blue(0, 0, 255);
const Color Color::White(255, 255, 255);
const Color Color::Black(0, 0, 0);
const Color Color::Transparent(0, 0, 0, 0);

Color Color::premultiplied() const noexcept {
    return {
        static_cast<uint8_t>(Clamp(static_cast<int>(r * static_cast<float>(a) / 255.0f), 0, 255)),
        static_cast<uint8_t>(Clamp(static_cast<int>(g * static_cast<float>(a) / 255.0f), 0, 255)),
        static_cast<uint8_t>(Clamp(static_cast<int>(b * static_cast<float>(a) / 255.0f), 0, 255)),
        a
    };
}

Color Color::linearToSrgb() const noexcept {
    auto linearToSrgbComponent = [](float c) {
        if (c <= 0.0031308f) {
            return 12.92f * c;
        } else {
            return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }
    };

    return {
        static_cast<uint8_t>(Clamp(static_cast<int>(linearToSrgbComponent(static_cast<float>(r) / 255.0f) * 255.0f), 0, 255)),
        static_cast<uint8_t>(Clamp(static_cast<int>(linearToSrgbComponent(static_cast<float>(g) / 255.0f) * 255.0f), 0, 255)),
        static_cast<uint8_t>(Clamp(static_cast<int>(linearToSrgbComponent(static_cast<float>(b) / 255.0f) * 255.0f), 0, 255)),
        a
    };
}

Color Color::srgbToLinear() const noexcept {
    auto srgbToLinearComponent = [](float c) {
        if (c <= 0.04045f) {
            return c / 12.92f;
        } else {
            return std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
    };

    return {
        static_cast<uint8_t>(Clamp(static_cast<int>(srgbToLinearComponent(static_cast<float>(r) / 255.0f) * 255.0f), 0, 255)),
        static_cast<uint8_t>(Clamp(static_cast<int>(srgbToLinearComponent(static_cast<float>(g) / 255.0f) * 255.0f), 0, 255)),
        static_cast<uint8_t>(Clamp(static_cast<int>(srgbToLinearComponent(static_cast<float>(b) / 255.0f) * 255.0f), 0, 255)),
        a
    };
}

Color Color::fromHSV(float h, float s, float v, uint8_t a) noexcept {
    if (s == 0.0f) {
        uint8_t value = static_cast<uint8_t>(Clamp(static_cast<int>(v * 255.0f), 0, 255));
        return {value, value, value, a};
    }

    h *= 6.0f;
    int i = static_cast<int>(std::floor(h));
    float f = h - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));

    uint8_t value_v = static_cast<uint8_t>(Clamp(static_cast<int>(v * 255.0f), 0, 255));
    uint8_t value_p = static_cast<uint8_t>(Clamp(static_cast<int>(p * 255.0f), 0, 255));
    uint8_t value_q = static_cast<uint8_t>(Clamp(static_cast<int>(q * 255.0f), 0, 255));
    uint8_t value_t = static_cast<uint8_t>(Clamp(static_cast<int>(t * 255.0f), 0, 255));

    switch (i) {
        case 0: return {value_v, value_t, value_p, a};
        case 1: return {value_q, value_v, value_p, a};
        case 2: return {value_p, value_v, value_t, a};
        case 3: return {value_p, value_q, value_v, a};
        case 4: return {value_t, value_p, value_v, a};
        default: return {value_v, value_p, value_q, a};
    }
}

Color Color::fromHex(uint32_t hex) noexcept {
    uint8_t r = static_cast<uint8_t>((hex >> 16) & 0xFF);
    uint8_t g = static_cast<uint8_t>((hex >> 8) & 0xFF);
    uint8_t b = static_cast<uint8_t>(hex & 0xFF);
    uint8_t a = 255;
    if((hex >> 24) & 0xFF)
        a = static_cast<uint8_t>((hex >> 24) & 0xFF);
    return {r, g, b, a};
}

} // namespace Math
} // namespace Sleak