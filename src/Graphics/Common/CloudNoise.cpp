#include "../../include/private/Graphics/Common/CloudNoise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace Sleak {
namespace RenderEngine {

namespace {

// Gradient-noise fBm: 32, 64, 128 and 256 lattice cells across the tile
constexpr uint32_t OCTAVES = 4;
constexpr uint32_t BASE_CELLS = 32;
constexpr double GAIN = 0.5;
// BSL's noise statistics: mean 0.48, standard deviation 0.10
constexpr double MEAN = 0.48;
constexpr double SPREAD = 0.10;
constexpr double DEVIATION_MIN = 1e-12;
// pcg3d, the star hash of sleak/atmosphere.glsl
constexpr uint32_t PCG_MUL = 1664525u;
constexpr uint32_t PCG_INC = 1013904223u;
constexpr uint32_t PCG_SHIFT = 16u;
// A 32-bit hash to an angle
constexpr double HASH_TO_RADIANS = 2.0 * std::numbers::pi / 4294967296.0;
// Quintic fade 6t^5 - 15t^4 + 10t^3
constexpr double FADE_A = 6.0, FADE_B = 15.0, FADE_C = 10.0;
constexpr double TEXEL_CENTRE = 0.5;
constexpr uint32_t CHANNELS = 4;
constexpr uint32_t LEVELS = 256;
constexpr double LEVEL_MAX = 255.0;

/// Tile texels and, per 8-bit level, how many texels are at or above it.
struct Tile {
    std::vector<uint8_t> texels;
    std::array<uint32_t, LEVELS> atOrAbove{};
};

/// First lane of pcg3d (Jarzynski and Olano 2020).
uint32_t Hash(uint32_t x, uint32_t y, uint32_t z) {
    x = x * PCG_MUL + PCG_INC;
    y = y * PCG_MUL + PCG_INC;
    z = z * PCG_MUL + PCG_INC;
    x += y * z;
    y += z * x;
    z += x * y;
    x ^= x >> PCG_SHIFT;
    y ^= y >> PCG_SHIFT;
    z ^= z >> PCG_SHIFT;
    return x + y * z;
}

/// Quintic fade of a lattice fraction.
double Fade(double t) {
    return t * t * t * (t * (t * FADE_A - FADE_B) + FADE_C);
}

/// Adds one octave of tiling gradient noise times amplitude to acc.
void AddOctave(uint32_t cells, uint32_t octave, double amplitude,
               std::vector<double>& acc) {
    // Unit gradient per lattice point, hashed from (x, y, octave)
    std::vector<double> gx(cells * cells), gy(cells * cells);
    for (uint32_t j = 0; j < cells; ++j) {
        for (uint32_t i = 0; i < cells; ++i) {
            const double a = Hash(i, j, octave) * HASH_TO_RADIANS;
            gx[j * cells + i] = std::cos(a);
            gy[j * cells + i] = std::sin(a);
        }
    }
    const auto corner = [&](uint32_t i, uint32_t j, double dx, double dy) {
        return gx[j * cells + i] * dx + gy[j * cells + i] * dy;
    };
    const double scale = static_cast<double>(cells) / CLOUD_NOISE_SIZE;
    for (uint32_t y = 0; y < CLOUD_NOISE_SIZE; ++y) {
        const double v = (y + TEXEL_CENTRE) * scale;
        const auto y0 = static_cast<uint32_t>(v);
        const uint32_t y1 = (y0 + 1) % cells;
        const double fy = v - y0;
        for (uint32_t x = 0; x < CLOUD_NOISE_SIZE; ++x) {
            const double u = (x + TEXEL_CENTRE) * scale;
            const auto x0 = static_cast<uint32_t>(u);
            const uint32_t x1 = (x0 + 1) % cells;
            const double fx = u - x0;
            const double sx = Fade(fx);
            const double low = std::lerp(corner(x0, y0, fx, fy),
                                         corner(x1, y0, fx - 1.0, fy), sx);
            const double high =
                std::lerp(corner(x0, y1, fx, fy - 1.0),
                          corner(x1, y1, fx - 1.0, fy - 1.0), sx);
            acc[y * CLOUD_NOISE_SIZE + x] +=
                amplitude * std::lerp(low, high, Fade(fy));
        }
    }
}

/// fBm remapped to BSL's statistics, 8 bits per texel, and its histogram.
Tile BuildTile() {
    const size_t count =
        static_cast<size_t>(CLOUD_NOISE_SIZE) * CLOUD_NOISE_SIZE;
    std::vector<double> acc(count, 0.0);
    double amplitude = 1.0;
    for (uint32_t octave = 0; octave < OCTAVES; ++octave) {
        AddOctave(BASE_CELLS << octave, octave, amplitude, acc);
        amplitude *= GAIN;
    }
    double sum = 0.0, squares = 0.0;
    for (double v : acc) {
        sum += v;
        squares += v * v;
    }
    const double mean = sum / count;
    const double deviation =
        std::sqrt(std::max(squares / count - mean * mean, DEVIATION_MIN));
    Tile tile;
    tile.texels.resize(count * CHANNELS);
    std::array<uint32_t, LEVELS> histogram{};
    for (size_t i = 0; i < count; ++i) {
        const double v =
            std::clamp(MEAN + SPREAD * (acc[i] - mean) / deviation, 0.0, 1.0);
        const auto level = static_cast<uint8_t>(std::lround(v * LEVEL_MAX));
        std::fill_n(&tile.texels[i * CHANNELS], CHANNELS, level);
        ++histogram[level];
    }
    uint32_t running = 0;
    for (uint32_t level = LEVELS; level-- > 0;) {
        running += histogram[level];
        tile.atOrAbove[level] = running;
    }
    return tile;
}

/// The tile, built once.
const Tile& GetTile() {
    static const Tile tile = BuildTile();
    return tile;
}

}  // namespace

const std::vector<uint8_t>& CloudNoiseTexels() { return GetTile().texels; }

float CloudNoiseThreshold(float coverage) {
    const Tile& tile = GetTile();
    const double target = std::clamp(coverage, 0.0f, 1.0f) *
                          static_cast<double>(tile.atOrAbove[0]);
    uint32_t level = LEVELS - 1;
    while (level > 0 && tile.atOrAbove[level] < target) --level;
    // atOrAbove[level] >= target > atOrAbove[level + 1]: place it in the level
    const double above = level + 1 < LEVELS ? tile.atOrAbove[level + 1] : 0.0;
    const double inLevel = (tile.atOrAbove[level] - target) /
                           std::max(tile.atOrAbove[level] - above, 1.0);
    return static_cast<float>((level + inLevel) / LEVEL_MAX);
}

}  // namespace RenderEngine
}  // namespace Sleak
