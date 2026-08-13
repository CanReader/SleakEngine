#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <random>

namespace Sleak {
namespace RenderEngine {

// Generates a hemisphere sample kernel for SSAO.
// Samples are distributed in a unit hemisphere (z >= 0) with
// cosine-weighted distribution biased toward the center.
struct SSAOKernel {
    static constexpr uint32_t MAX_KERNEL_SIZE = 64;

    /// One hemisphere sample offset (w unused, kept for GPU alignment).
    struct Sample {
        float x, y, z, _pad;
    };

    std::array<Sample, MAX_KERNEL_SIZE> samples{};

    // 4x4 noise texture data (random rotations for kernel)
    static constexpr uint32_t NOISE_SIZE = 4;
    std::array<float, NOISE_SIZE * NOISE_SIZE * 4> noiseData{};

    /// Fills samples[] and noiseData[] with a deterministic (seed 42) cosine-weighted kernel.
    void Generate(uint32_t kernelSize = MAX_KERNEL_SIZE) {
        std::mt19937 rng(42); // deterministic seed for reproducibility
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        std::uniform_real_distribution<float> ndist(-1.0f, 1.0f);

        for (uint32_t i = 0; i < kernelSize; ++i) {
            // Random point in unit hemisphere
            float x = ndist(rng);
            float y = ndist(rng);
            float z = dist(rng); // z in [0, 1] — hemisphere

            // Normalize
            float len = std::sqrt(x * x + y * y + z * z);
            if (len < 0.0001f) len = 0.0001f;
            x /= len;
            y /= len;
            z /= len;

            // Scale with accelerating interpolation (more samples near origin)
            float scale = static_cast<float>(i) / static_cast<float>(kernelSize);
            scale = Lerp(0.1f, 1.0f, scale * scale);
            x *= scale;
            y *= scale;
            z *= scale;

            samples[i] = {x, y, z, 0.0f};
        }

        // Generate 4x4 noise texture (random tangent-space rotations around z)
        for (uint32_t i = 0; i < NOISE_SIZE * NOISE_SIZE; ++i) {
            noiseData[i * 4 + 0] = ndist(rng); // x rotation
            noiseData[i * 4 + 1] = ndist(rng); // y rotation
            noiseData[i * 4 + 2] = 0.0f;        // z = 0 (rotate around z)
            noiseData[i * 4 + 3] = 0.0f;        // pad
        }
    }

private:
    static float Lerp(float a, float b, float t) {
        return a + t * (b - a);
    }
};

} // namespace RenderEngine
} // namespace Sleak
