#ifndef _OUTPUT_TRANSFORM_HPP_
#define _OUTPUT_TRANSFORM_HPP_

#include <cstdint>

namespace Sleak {
namespace RenderEngine {

/// Settings the output transform is built from (Renderer state).
struct OutputTransformSettings {
    uint32_t op = 0;  // TonemapOperator
    float exposure = 1.0f;
    float gamma = 1.0f;  // 1 = no display encode
    float saturation = 1.0f;
    bool dither = false;
    float whiteCurve = 2.0f;
    float whitePath = 1.0f;
};

/// Output transform as shaders read it (sleak/tonemap.glsl SleakOutputParams,
/// FrameData Output/OutputCurve/WhitePath*, the composite push constants).
struct OutputTransformParams {
    uint32_t op = 0;
    float exposure = 1.0f;
    float gamma = 1.0f;
    float saturation = 1.0f;
    float dither = 0.0f;
    float curve = 2.0f;
    float white[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    float whiteInverse[4] = {1.0f, 0.0f, 0.0f, 1.0f};
};

/// Clamps the settings and derives the white-path matrix and its inverse.
OutputTransformParams BuildOutputTransform(const OutputTransformSettings& s);

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // _OUTPUT_TRANSFORM_HPP_
