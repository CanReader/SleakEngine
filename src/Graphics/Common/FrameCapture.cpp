#include "../../include/private/Graphics/Common/FrameCapture.hpp"

#include <Core/Logger.hpp>
#include <fstream>

namespace Sleak {
namespace RenderEngine {

bool WriteFramePPM(const std::string& path, uint32_t width, uint32_t height,
                   const std::vector<uint8_t>& rgb) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()),
              static_cast<std::streamsize>(rgb.size()));
    if (!out) {
        SLEAK_ERROR("Frame capture: could not write {}", path);
        return false;
    }
    SLEAK_INFO("Frame capture: wrote {}x{} to {}", width, height, path);
    return true;
}

}  // namespace RenderEngine
}  // namespace Sleak
