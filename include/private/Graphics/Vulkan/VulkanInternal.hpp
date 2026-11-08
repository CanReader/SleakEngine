#ifndef _VULKANINTERNAL_HPP_
#define _VULKANINTERNAL_HPP_

#include <algorithm>
#include <cmath>

#include "../Common/MatrixMath.hpp"

namespace Sleak {
namespace RenderEngine {

/// Halton low-discrepancy sequence value for TAA sub-pixel jitter.
inline float HaltonSeq(int index, int base) {
    float result = 0.0f;
    float f = 1.0f;
    for (int i = index; i > 0; i /= base) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(i % base);
    }
    return result;
}

}  // namespace RenderEngine
}  // namespace Sleak

#endif
