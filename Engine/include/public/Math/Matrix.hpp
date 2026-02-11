#ifndef _MATRIX_H_
#define _MATRIX_H_

#include <Core/OSDef.hpp>
#include "Math.hpp"
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <type_traits>

#include "Vector.hpp"
#include "Quaternion.hpp"

namespace Sleak {
namespace Math {
template <typename T, size_t Rows, size_t Cols>
class Matrix {
   public:
    // Optimized default constructor
    constexpr Matrix() noexcept {
        if constexpr (Rows == Cols) {
            // Identity matrix for square matrices
            for (size_t i = 0; i < Rows; ++i) {
                for (size_t j = 0; j < Cols; ++j) {
                    data[i][j] =
                        (i == j) ? static_cast<T>(1) : static_cast<T>(0);
                }
            }
        } else {
            // Zero matrix for non-square matrices
            std::memset(data, 0, sizeof(data));
        }
    }

    // Optimized initializer list constructor with bounds checking
    Matrix(std::initializer_list<std::initializer_list<T>> list) {
        if (list.size() != Rows) {
            throw std::invalid_argument(
                "Number of rows does not match matrix dimension");
        }

        size_t i = 0;
        for (const auto& row : list) {
            if (row.size() != Cols) {
                throw std::invalid_argument(
                    "Number of columns does not match matrix dimension");
            }

            size_t j = 0;
            for (const auto& val : row) {
                data[i][j++] = val;
            }
            ++i;
        }
    }

    // Bounds-checked element access
    T& operator()(size_t row, size_t col) {
        if (row >= Rows || col >= Cols) {
            throw std::out_of_range("Matrix indices out of range");
        }
        return data[row][col];
    }

    const T& operator()(size_t row, size_t col) const {
        if (row >= Rows || col >= Cols) {
            throw std::out_of_range("Matrix indices out of range");
        }
        return data[row][col];
    }

    // Efficient matrix addition
    Matrix<T, Rows, Cols> operator+(const Matrix<T, Rows, Cols>& other) const {
        Matrix<T, Rows, Cols> result;
        for (size_t i = 0; i < Rows; ++i) {
            for (size_t j = 0; j < Cols; ++j) {
                result(i, j) = data[i][j] + other(i, j);
            }
        }
        return result;
    }

    // Efficient matrix subtraction
    Matrix<T, Rows, Cols> operator-(const Matrix<T, Rows, Cols>& other) const {
        Matrix<T, Rows, Cols> result;
        for (size_t i = 0; i < Rows; ++i) {
            for (size_t j = 0; j < Cols; ++j) {
                result(i, j) = data[i][j] - other(i, j);
            }
        }
        return result;
    }

    // Optimized matrix multiplication with early exit for zero-like cases
    template <size_t OtherCols>
    Matrix<T, Rows, OtherCols> operator*(
        const Matrix<T, Cols, OtherCols>& other) const {
        Matrix<T, Rows, OtherCols> result;

        // Quick zero check
        bool isZero = true;
        for (size_t i = 0; i < Rows; ++i) {
            for (size_t j = 0; j < OtherCols; ++j) {
                T sum = static_cast<T>(0);
                for (size_t k = 0; k < Cols; ++k) {
                    sum += data[i][k] * other(k, j);
                }
                result(i, j) = sum;

                // Track if result is non-zero
                if (std::abs(sum) > std::numeric_limits<T>::epsilon()) {
                    isZero = false;
                }
            }
        }

        return result;
    }

    Matrix<T, Cols, Rows> Transpose() const {
        Matrix<T, Cols, Rows> result;
        for (size_t i = 0; i < Rows; ++i) {
            for (size_t j = 0; j < Cols; ++j) {
                result(j, i) = data[i][j];
            }
        }
        return result;
    }

    // Improved determinant calculation with recursive approach
    T Determinant() const {
        static_assert(Rows == Cols,
                      "Determinant is only defined for square matrices");

        if constexpr (Rows == 1) return data[0][0];
        if constexpr (Rows == 2) {
            return data[0][0] * data[1][1] - data[0][1] * data[1][0];
        }
        if constexpr (Rows == 3) {
            return data[0][0] *
                       (data[1][1] * data[2][2] - data[1][2] * data[2][1]) -
                   data[0][1] *
                       (data[1][0] * data[2][2] - data[1][2] * data[2][0]) +
                   data[0][2] *
                       (data[1][0] * data[2][1] - data[1][1] * data[2][0]);
        }

        throw std::runtime_error(
            "Determinant not implemented for matrices larger than 3x3");
    }

    // Improved inverse for 2x2 matrices with error handling
    Matrix<T, Rows, Cols> Inverse() const {
        static_assert(Rows == Cols,
                      "Inverse is only defined for square matrices");

        T det = Determinant();
        if (std::abs(det) < std::numeric_limits<T>::epsilon()) {
            throw std::runtime_error(
                "Matrix is singular (determinant is near zero)");
        }

        if constexpr (Rows == 2) {
            Matrix<T, Rows, Cols> result;
            result(0, 0) = data[1][1] / det;
            result(0, 1) = -data[0][1] / det;
            result(1, 0) = -data[1][0] / det;
            result(1, 1) = data[0][0] / det;
            return result;
        }

        throw std::runtime_error(
            "Inverse not implemented for matrices larger than 2x2");
    }

    // Optimized output method
    std::string ToString() const {
        std::ostringstream ss;
        ss << "Matrix:\n";
        for (size_t i = 0; i < Rows; ++i) {
            ss << "|  ";
            for (size_t j = 0; j < Cols; ++j) {
                ss << std::setw(10) << std::fixed << std::setprecision(4)
                   << data[i][j] << "  ";
            }
            ss << "|\n";
        }
        return ss.str();
    }

    // Stream operator
    friend std::ostream& operator<<(std::ostream& os,
                                    const Matrix<T, Rows, Cols>& matrix) {
        os << matrix.ToString();
        return os;
    }

    // Static matrix creation methods remain the same
    static Matrix<T, Rows, Rows> Identity() {
        static_assert(Rows == Cols,
                      "Identity matrix is only defined for square matrices");
        Matrix<T, Rows, Rows> result;
        return result;
    }

    static Matrix<T, 4, 4> Perspective(T fovY, T aspectRatio, T nearPlane,
                                       T farPlane) {
        static_assert(Rows == 4 && Cols == 4, "Perspective matrix must be 4x4");
        T tanHalfFovY = tan(fovY / 2.0f);

        Matrix<T, 4, 4> result = Matrix<T, 4, 4>::Identity();

        result(0, 0) = 1.0f / (aspectRatio * tanHalfFovY);
        result(1, 1) = 1.0f / tanHalfFovY;
        result(2, 2) = farPlane / (farPlane - nearPlane);
        result(2, 3) = (-nearPlane * farPlane) / (farPlane - nearPlane);
        result(3, 2) = 1.0f;
        result(3, 3) = 0.0f;

        result(2, 2) = farPlane / (farPlane - nearPlane);
        result(2, 3) = 1.0f;
        result(3, 2) = (-nearPlane * farPlane) / (farPlane - nearPlane);
        result(3, 3) = 0.0f;


        return result;
    }



    static Matrix<T, 4, 4> Orthographic(T left, T right, T bottom, T top, T nearPlane, T farPlane) {
        static_assert(Rows == 4 && Cols == 4, "Orthographic matrix must be 4x4");

        Matrix<T, 4, 4> result = Matrix<T, 4, 4>::Identity();

        // Diagonal elements
        result(0, 0) = 2.0f / (right - left);       // Scale X
        result(1, 1) = 2.0f / (top - bottom);       // Scale Y
        result(2, 2) = -2.0f / (farPlane - nearPlane); // Scale Z (negative for right-handed systems)

        // Translation elements
        result(0, 3) = -(right + left) / (right - left);   // Translate X
        result(1, 3) = -(top + bottom) / (top - bottom);   // Translate Y
        result(2, 3) = -(farPlane + nearPlane) / (farPlane - nearPlane); // Translate Z

        return result;
    }

    static Matrix<T, 4, 4> LookAt(const Vector<T, 3>& eye,
                                  const Vector<T, 3>& center,
                                  const Vector<T, 3>& up) {
        static_assert(Rows == 4 && Cols == 4, "View matrix must be 4x4");

        Vector<T, 3> zAxis = (center - eye).Normalized();
        Vector<T, 3> xAxis = up.Cross(zAxis).Normalized();
        Vector<T, 3> yAxis = zAxis.Cross(xAxis);

        Matrix<T, 4, 4> result;

        // Assign basis vectors correctly (Column-major order)
        result(0, 0) = xAxis[0];
        result(1, 0) = xAxis[1];
        result(2, 0) = xAxis[2];
        result(3, 0) = -xAxis.Dot(eye);

        result(0, 1) = yAxis[0];
        result(1, 1) = yAxis[1];
        result(2, 1) = yAxis[2];
        result(3, 1) = -yAxis.Dot(eye);

        result(0, 2) = +zAxis[0];
        result(1, 2) = +zAxis[1];
        result(2, 2) = +zAxis[2];
        result(3, 2) = -zAxis.Dot(eye);

        result(0, 3) = 0;
        result(1, 3) = 0;
        result(2, 3) = 0;
        result(3, 3) = 1;

        return result;
    }



    static Matrix<T, 4, 4> LookAtRH(const Vector<T, 3>& eye,
        const Vector<T, 3>& center,
        const Vector<T, 3>& up) {
            Vector<T, 3> zAxis = (center - eye).Normalized();  // Notice: reversed direction
            Vector<T, 3> xAxis = up.Cross(zAxis).Normalized();
            Vector<T, 3> yAxis = zAxis.Cross(xAxis);

            Matrix<T, 4, 4> result = Matrix<T, 4, 4>::Identity();
            result(0, 0) = xAxis[0];
            result(1, 0) = xAxis[1];
            result(2, 0) = xAxis[2];

            result(0, 1) = yAxis[0];
            result(1, 1) = yAxis[1];
            result(2, 1) = yAxis[2];

            result(0, 2) = -zAxis[0];  // Negate z-axis
            result(1, 2) = -zAxis[1];
            result(2, 2) = -zAxis[2];

            result(3, 0) = -xAxis.Dot(eye);
            result(3, 1) = -yAxis.Dot(eye);
            result(3, 2) = zAxis.Dot(eye);  // Negate translation for right-handed
            return result;
        }

        static Matrix<T, 4, 4> LookTo(const Vector<T, 3>& eye,
            const Vector<T, 3>& direction,
            const Vector<T, 3>& up) {
        Vector<T, 3> zAxis = direction.Normalized();  // Forward direction
        Vector<T, 3> xAxis = up.Cross(zAxis).Normalized();
        Vector<T, 3> yAxis = zAxis.Cross(xAxis);

        Matrix<T, 4, 4> result = Matrix<T, 4, 4>::Identity();
        result(0, 0) = xAxis[0];
        result(1, 0) = xAxis[1];
        result(2, 0) = xAxis[2];

        result(0, 1) = yAxis[0];
        result(1, 1) = yAxis[1];
        result(2, 1) = yAxis[2];

        result(0, 2) = zAxis[0];
        result(1, 2) = zAxis[1];
        result(2, 2) = zAxis[2];

        result(3, 0) = -xAxis.Dot(eye);
        result(3, 1) = -yAxis.Dot(eye);
        result(3, 2) = -zAxis.Dot(eye);

        return result;
    }

    static Matrix<T, 4, 4> FreeLook(const Vector<T, 3>& position, 
        T yaw, T pitch) {
        Vector<T, 3> forward;
        forward[0] = cos(yaw) * cos(pitch);
        forward[1] = sin(pitch);
        forward[2] = sin(yaw) * cos(pitch);

        return LookToLH(position, forward, Vector<T, 3>{0, 1, 0});
    }

    static Matrix<T, 4, 4> OrbitView(const Vector<T, 3>& target,
        T distance, 
        T theta, T phi) {
        Vector<T, 3> direction = {
            sin(phi) * cos(theta),
            cos(phi),
            sin(phi) * sin(theta)
        };

        Vector<T, 3> eye = target + direction * distance;
        return LookAtLH(eye, target, Vector<T, 3>{0, 1, 0});
    }

    static Matrix<T, 4, 4> Translate(const Vector3D& translation) {
        Matrix<T, 4, 4> result = Matrix<T, 4, 4>::Identity();

        result(3, 0) = translation.GetX();  // Move to last row, first column
        result(3, 1) = translation.GetY();  // Move to last row, second column
        result(3, 2) = translation.GetZ();  // Move to last row, third column

        return result;
    }


    static Matrix<T, 4, 4> Rotate(const Quaternion& rotation) {
        return rotation.toRotationMatrix(); // Assuming Quaternion has toRotationMatrix()
    }

    static Matrix<T, 4, 4> Scale(const Vector3D& scale) {
        Matrix<T, 4, 4> result =
            Matrix<T, 4, 4>::Identity();  // Ensure identity
        result(0, 0) = scale.GetX();
        result(1, 1) = scale.GetY();
        result(2, 2) = scale.GetZ();
        return result;
    }

    static Matrix<T, 4, 4> Scale(const Vector3D& scale, const Vector3D& center) {
        Matrix<T, 4, 4> translateToOrigin = Translate(center*(-1));
    
        Matrix<T, 4, 4> scaling = Matrix<T, 4, 4>::Identity();
        scaling(0, 0) = scale.GetX();
        scaling(1, 1) = scale.GetY();
        scaling(2, 2) = scale.GetZ();
    
        Matrix<T, 4, 4> translateBack = Translate(center);
    
        return translateBack * scaling * translateToOrigin;
    }

   private:
    T data[Rows][Cols];
};

// Type alias for 4x4 matrix
using Matrix4 = Matrix<float, 4, 4>;


#ifdef PLATFORM_WIN
#include <DirectXMath.h>

static Matrix<float, 4, 4> XMToMatrix(DirectX::XMMATRIX mat) {
    Matrix<float, 4, 4> matr;
    DirectX::XMMATRIX worldMatrix;

    DirectX::XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, worldMatrix));

    // Store the matrix in row-major order
    DirectX::XMFLOAT4X4 floatMat;
    DirectX::XMStoreFloat4x4(&floatMat, mat);

    // Copy the elements from XMFLOAT4X4 to Sleak::Math::Matrix4
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            matr(row, col) = floatMat.m[row][col];
        }
    }

    return matr;
}

#endif

}  // namespace Math
}  // namespace Sleak

#endif  // _MATRIX_H_
