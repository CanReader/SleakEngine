#ifndef _FRAME_CAPTURE_HPP_
#define _FRAME_CAPTURE_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace Sleak {
namespace RenderEngine {

/// Bytes per pixel of the RGB rows WriteFramePPM takes.
inline constexpr uint32_t FRAME_CAPTURE_CHANNELS = 3;

/// Writes tightly packed 8-bit RGB rows, top row first, as a binary PPM and
/// logs the result. Shared by every backend's -capture readback.
bool WriteFramePPM(const std::string& path, uint32_t width, uint32_t height,
                   const std::vector<uint8_t>& rgb);

}  // namespace RenderEngine
}  // namespace Sleak

#endif
