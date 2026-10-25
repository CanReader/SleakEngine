#ifndef _VULKANINTERNAL_HPP_
#define _VULKANINTERNAL_HPP_

#include <cmath>
#include <algorithm>

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

/// Row-major mat4 multiply: C = A * B.
template <typename T>
void MatMul4(const T A[16], const T B[16], T C[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            T s = T(0);
            for (int k = 0; k < 4; ++k) s += A[r * 4 + k] * B[k * 4 + c];
            C[r * 4 + c] = s;
        }
}

/// Row-major 4x4 inverse via Gauss-Jordan elimination.
template <typename T>
bool InvertMat4(const T M[16], T out[16]) {
    constexpr T kSingularEpsilon = T(1e-7);
    T m[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) m[r][c]     = M[r * 4 + c];
        for (int c = 0; c < 4; ++c) m[r][4 + c] = (r == c) ? T(1) : T(0);
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = -1; T maxv = T(0);
        for (int row = col; row < 4; ++row) {
            T v = std::abs(m[row][col]);
            if (v > maxv) { maxv = v; pivot = row; }
        }
        if (pivot < 0 || maxv < kSingularEpsilon) return false;
        if (pivot != col) std::swap(m[col], m[pivot]);
        T inv = T(1) / m[col][col];
        for (int c = 0; c < 8; ++c) m[col][c] *= inv;
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            T f = m[row][col];
            for (int c = 0; c < 8; ++c) m[row][c] -= f * m[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = m[r][4 + c];
    return true;
}

}  // namespace RenderEngine
}  // namespace Sleak

#endif
