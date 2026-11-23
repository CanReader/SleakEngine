#ifndef _CLOUD_NOISE_HPP_
#define _CLOUD_NOISE_HPP_

#include <cstdint>
#include <vector>

namespace Sleak {
namespace RenderEngine {

/// Edge of the square cloud noise tile in texels (sleak/clouds.glsl).
inline constexpr uint32_t CLOUD_NOISE_SIZE = 512;

/// RGBA8 texels of the tiling cloud noise, the value in every channel;
/// built on first use, the same bytes on every run of a build.
const std::vector<uint8_t>& CloudNoiseTexels();

/// Noise value in [0, 1] that a share of the tile's texels exceeds.
float CloudNoiseThreshold(float coverage);

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _CLOUD_NOISE_HPP_
