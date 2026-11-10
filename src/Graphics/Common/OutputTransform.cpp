#include "../../include/private/Graphics/Common/OutputTransform.hpp"

#include <algorithm>
#include <cmath>

namespace Sleak {
namespace RenderEngine {

namespace {

// White-path crosstalk per unit of tonemapWhitePath: neighbour, far channel
constexpr double WHITE_NEAR = 0.03;
constexpr double WHITE_FAR = 0.01;
constexpr float WHITE_PATH_MAX = 4.0f;
// Soft curve exponent range; below 1 the curve stops being a shoulder
constexpr float CURVE_MIN = 1.0f;
constexpr float CURVE_MAX = 8.0f;
constexpr float GAMMA_MIN = 0.01f;
constexpr float SATURATION_MAX = 2.0f;
// One step of the 8-bit present target
constexpr float DITHER_LSB = 1.0f / 255.0f;
constexpr double DETERMINANT_MIN = 1e-9;
constexpr int DIM = 3;

/// Entries c, a, b, d of the symmetric white-path matrix.
void WhitePathEntries(double s, double out[4]) {
    const double a = WHITE_NEAR * s;
    const double b = WHITE_FAR * s;
    out[0] = 1.0 - a - b;
    out[1] = a;
    out[2] = b;
    out[3] = 1.0 - 2.0 * a;
}

/// Full 3x3 matrix from the four distinct entries.
void Expand(const double e[4], double m[DIM][DIM]) {
    const double rows[DIM][DIM] = {
        {e[0], e[1], e[2]}, {e[1], e[3], e[1]}, {e[2], e[1], e[0]}};
    for (int r = 0; r < DIM; ++r)
        for (int c = 0; c < DIM; ++c) m[r][c] = rows[r][c];
}

/// Cofactor inverse of a 3x3 matrix; false when it is singular.
bool Invert(const double m[DIM][DIM], double inv[DIM][DIM]) {
    double cof[DIM][DIM];
    for (int r = 0; r < DIM; ++r) {
        for (int c = 0; c < DIM; ++c) {
            const int r0 = (r + 1) % DIM, r1 = (r + 2) % DIM;
            const int c0 = (c + 1) % DIM, c1 = (c + 2) % DIM;
            cof[r][c] = m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0];
        }
    }
    const double det =
        m[0][0] * cof[0][0] + m[0][1] * cof[0][1] + m[0][2] * cof[0][2];
    if (std::abs(det) < DETERMINANT_MIN) return false;
    for (int r = 0; r < DIM; ++r)
        for (int c = 0; c < DIM; ++c) inv[r][c] = cof[c][r] / det;
    return true;
}

}  // namespace

OutputTransformParams BuildOutputTransform(const OutputTransformSettings& s) {
    OutputTransformParams p;
    p.op = s.op;
    p.exposure = std::max(s.exposure, 0.0f);
    p.gamma = std::max(s.gamma, GAMMA_MIN);
    p.saturation = std::clamp(s.saturation, 0.0f, SATURATION_MAX);
    p.dither = s.dither ? DITHER_LSB : 0.0f;
    p.curve = std::clamp(s.whiteCurve, CURVE_MIN, CURVE_MAX);

    double entries[4];
    WhitePathEntries(std::clamp(s.whitePath, 0.0f, WHITE_PATH_MAX), entries);
    double m[DIM][DIM], inv[DIM][DIM];
    Expand(entries, m);
    if (!Invert(m, inv)) return p;
    const double invEntries[4] = {inv[0][0], inv[0][1], inv[0][2], inv[1][1]};
    for (int i = 0; i < 4; ++i) {
        p.white[i] = static_cast<float>(entries[i]);
        p.whiteInverse[i] = static_cast<float>(invEntries[i]);
    }
    return p;
}

}  // namespace RenderEngine
}  // namespace Sleak
