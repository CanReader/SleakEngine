#ifndef _Vector_HPP_
#define _Vector_HPP_

#include <cstdint>
#include <string>
#include <sstream>
#include <cmath>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <cassert>
#include <iomanip>

#define VECTOR_Up       Vector3D(0, 1, 0)
#define VECTOR_Down     Vector3D(0, -1, 0)
#define VECTOR_Right    Vector3D(1, 0, 0)
#define VECTOR_Left     Vector3D(-1, 0, 0)
#define VECTOR_Forward  Vector3D(0, 0, 1)
#define VECTOR_Backward Vector3D(0, 0, -1)

#define EPSILON 1e-5f

namespace Sleak 
{
    namespace Math
    {
        /// Fixed-size numeric vector with the standard component-wise algebra.
        /// Vector2D/3D/4D wrap this for the common dimensions used engine-wide.
        /// @ingroup math
        template <typename T, size_t N>
        class Vector {
        public:
            // Constructors
            constexpr Vector() noexcept { data.fill(static_cast<T>(0)); }

            constexpr Vector(std::initializer_list<T> list) {
                if (list.size() != N) {
                    throw std::invalid_argument(
                        "Initializer list size does not match vector "
                        "dimension");
                }
                std::copy_n(list.begin(), N, data.begin());
            }
    
            // Access elements (read/write)
            [[nodiscard]] T& operator[](size_t index) {
                assert(index < N && "Vector index out of range");
                return data[index];
            }

            // Read-only access
            [[nodiscard]] const T& operator[](size_t index) const {
                assert(index < N && "Vector index out of range");
                return data[index];
            }
    
            // Basic operations
            Vector<T, N> operator+(const Vector<T, N>& other) const {
                Vector<T, N> result;
                std::transform(data.begin(), data.end(), other.data.begin(),
                              result.data.begin(), std::plus<T>());
                return result;
            }

            Vector<T, N> operator-(const Vector<T, N>& other) const {
                Vector<T, N> result;
                std::transform(data.begin(), data.end(), other.data.begin(),
                              result.data.begin(), std::minus<T>());
                return result;
            }

            Vector<T, N>& operator+=(const Vector<T, N>& other) {
                std::transform(data.begin(), data.end(), other.data.begin(),
                              data.begin(), std::plus<T>());
                return *this;
            }

            Vector<T, N>& operator-=(const Vector<T, N>& other) {
                std::transform(data.begin(), data.end(), other.data.begin(),
                              data.begin(), std::minus<T>());
                return *this;
            }

            Vector<T, N> operator*(T scalar) const {
                Vector<T, N> result;
                std::transform(
                    data.begin(), data.end(), result.data.begin(),
                    [scalar](const T& val) { return val * scalar; });
                return result;
            }
    
            Vector<T, N> operator/(T scalar) const {
                if (scalar == 0) {
                    throw std::invalid_argument("Division by zero");
                }
                Vector<T, N> result;
                for (size_t i = 0; i < N; ++i) {
                    result[i] = data[i] / scalar;
                }
                return result;
            }
    
            // Dot product
            T Dot(const Vector<T, N>& other) const {
                T result = 0;
                for (size_t i = 0; i < N; ++i) {
                    result += data[i] * other[i];
                }
                return result;
            }
    
            // Cross product (only for 3D vectors)
            template <size_t M = N>
            typename std::enable_if<M == 3, Vector<T, N>>::type
            Cross(const Vector<T, N>& other) const {
                static_assert(N == 3, "Cross product is only defined for 3D vectors");
                return {
                    data[1] * other[2] - data[2] * other[1],
                    data[2] * other[0] - data[0] * other[2],
                    data[0] * other[1] - data[1] * other[0]
                };
            }
    
            // Magnitude (length)
            T Magnitude() const {
                T sum = 0;
                for (size_t i = 0; i < N; ++i) {
                    sum += data[i] * data[i];
                }
                return std::sqrt(sum);
            }
    
            // Normalize the vector (in-place)
            void Normalize() {
                T mag = Magnitude();
                if (mag == 0) {
                    data[0] = 0.0f; data[1] = 0.0f; data[2] = 0.0f;
                    return;
                    // TODO: throw std::runtime_error("Cannot normalize a zero vector");
                }
                *this = *this / mag;
            }
    
            // Return a normalized copy of the vector
            Vector<T, N> Normalized() const {
                Vector<T, N> result = *this;
                result.Normalize();
                return result;
            }

            // Equality comparison
            bool operator==(const Vector<T, N>& other) const {
                for (size_t i = 0; i < N; ++i) {
                    if (std::abs(data[i] - other[i]) >= EPSILON) {
                        return false;
                    }
                }
                return true;
            }
    
            // Inequality comparison
            bool operator!=(const Vector<T, N>& other) const {
                return !(*this == other);
            }
    
            // Convert to string
            std::string ToString() const {
                std::ostringstream ss;
                ss << "Vector<" << N << ">(";
                for (size_t i = 0; i < N; ++i) {
                    ss << data[i];
                    if (i < N - 1) ss << ", ";
                }
                ss << ")\n";
                return ss.str();
            }
    
            /// Raw pointer to the backing N-element array.
            T* ToRawArray() {
                return data.data();
            }

            friend std::ostream& operator<<(
                    std::ostream& os, const Vector<T, N>& v) {
                    os << v.ToString();
                    return os;
                }

        private:
            std::array<T, N> data; 
        };
    
        /// Two-component float vector, used for UVs, screen positions, and
        /// min/max ranges such as a camera controller's pitch limits.
        ///
        /// Wraps Vector<float, 2> with named accessors and the usual
        /// operators. Components are read with GetX()/GetY(), written with
        /// SetX()/SetY() or Set(), and accumulated with AddX()/AddY().
        ///
        /// @code{.cpp}
        /// Sleak::Math::Vector2D tiling(2.0f, 2.0f);
        /// material->SetTiling(tiling);
        ///
        /// // Pitch clamp expressed as a min/max pair
        /// controller->SetPitchRange(Sleak::Math::Vector2D(-89.0f, 89.0f));
        /// @endcode
        ///
        /// @see Vector3D, Vector4D, Vector
        /// @ingroup math
        class Vector2D {
        public:
            // Constructors
            constexpr Vector2D() : vec({0.0f, 0.0f}) {}
            constexpr Vector2D(float x, float y) : vec({x, y}) {}
    
            // Accessors
            constexpr float GetX() const { return vec[0]; }
            constexpr float GetY() const { return vec[1]; }
    
            // Mutators
            void SetX(float val) { vec[0] = val; }
            void SetY(float val) { vec[1] = val; }
            void Set(float x, float y) { vec[0] = x; vec[1] = y; }

            void AddX(float val) { vec[0] += val; }
            void AddY(float val) { vec[1] += val; }
            void Add(float x, float y) { vec[0] += x; vec[1] += y; }
    
            // Basic operations
            Vector2D operator+(const Vector2D& other) const {
                return Vector2D(vec[0] + other.vec[0], vec[1] + other.vec[1]);
            }
    
            Vector2D operator-(const Vector2D& other) const {
                return Vector2D(vec[0] - other.vec[0], vec[1] - other.vec[1]);
            }
    
            Vector2D& operator+=(const Vector2D& other) {
                vec[0] += other.vec[0];
                vec[1] += other.vec[1];
                return *this;
            }
    
            Vector2D& operator-=(const Vector2D& other) {
                vec[0] -= other.vec[0];
                vec[1] -= other.vec[1];
                return *this;
            }
    
            // Scalar operations
            Vector2D operator*(float scalar) const {
                return Vector2D(vec[0] * scalar, vec[1] * scalar);
            }
    
            Vector2D operator/(float scalar) const {
                if (scalar == 0) throw std::invalid_argument("Division by zero");
                return Vector2D(vec[0] / scalar, vec[1] / scalar);
            }
    
            // Vector operations
            float Dot(const Vector2D& other) const {
                return vec[0] * other.vec[0] + vec[1] * other.vec[1];
            }
    
            float Cross(const Vector2D& other) const {
                return vec[0] * other.vec[1] - vec[1] * other.vec[0];
            }
    
            // Magnitude and normalization
            float Magnitude() const {
                return std::hypot(vec[0], vec[1]);
            }
    
            void Normalize() {
                float mag = Magnitude();
                if (mag == 0) throw std::runtime_error("Cannot normalize a zero vector");
                vec[0] /= mag;
                vec[1] /= mag;
            }
    
            Vector2D Normalized() const {
                Vector2D result = *this;
                result.Normalize();
                return result;
            }
    
            // Equality comparison
            bool operator==(const Vector2D& other) const {
                return std::abs(vec[0] - other.vec[0]) < EPSILON &&
                       std::abs(vec[1] - other.vec[1]) < EPSILON;
            }
    
            bool operator!=(const Vector2D& other) const {
                return !(*this == other);
            }

            friend std::ostream& operator<< (std::ostream& os, const Vector2D& vec) {
                os << vec.ToString();
                return os;
            }
    
            std::string ToString() const {
                return ToString(2);
            }

            std::string ToString(uint8_t precision) const {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(precision);
                ss << "Vector2D(" << vec[0] << ", " << vec[1] << ")";
                return ss.str();
            }

            float* ToArray() {
                return vec.ToRawArray();
            }
    
        private:
            Vector<float, 2> vec;
        };
    
        /// Three-component float vector: the workhorse type for positions,
        /// directions, scales, normals, and velocities across the engine.
        ///
        /// Wraps Vector<float, 3> with named accessors, full arithmetic,
        /// and the geometric operations you need day to day: Dot(),
        /// Cross(), Magnitude(), Normalize() (in place) and Normalized()
        /// (returns a copy). Note that `operator*` with another Vector3D is
        /// componentwise, not a dot or cross product.
        ///
        /// Named constants cover the axis directions: Zero(), Identity(),
        /// Up(), Down(), Left(), Right(), Forward(), and Backward().
        ///
        /// @code{.cpp}
        /// using Sleak::Math::Vector3D;
        ///
        /// Vector3D camPos(-5.0f, 3.0f, -5.0f);
        /// Vector3D target(0.0f, 0.0f, 0.0f);
        ///
        /// Vector3D forward = (target - camPos).Normalized();
        /// float distance   = (target - camPos).Magnitude();
        ///
        /// // Build a right vector from forward and world up
        /// Vector3D right = forward.Cross(Vector3D::Up()).Normalized();
        ///
        /// // Facing test: positive means the target is in front
        /// bool inFront = forward.Dot(target - camPos) > 0.0f;
        ///
        /// // Move along a direction
        /// camPos += forward * (speed * deltaTime);
        /// @endcode
        ///
        /// @see Vector2D, Vector4D, Vector, Quaternion
        /// @ingroup math
        class Vector3D {
        public:
            // Constructors
            Vector3D() : vec({0.0f, 0.0f, 0.0f}) {}
            Vector3D(float x, float y, float z) : vec({x, y, z}) {}
    
            // Accessors
            float GetX() const { return vec[0]; }
            float GetY() const { return vec[1]; }
            float GetZ() const { return vec[2]; }
    
            // Mutators
            void SetX(float val) { vec[0] = val; }
            void SetY(float val) { vec[1] = val; }
            void SetZ(float val) { vec[2] = val; }
            void Set(float x, float y, float z) {
                vec[0] = x; vec[1] = y; vec[2] = z;
            }

            void AddX(float val) { vec[0] += val; }
            void AddY(float val) { vec[1] += val; }
            void AddZ(float val) { vec[2] += val; }
            void Add(float x, float y, float z) {
                vec[0] += x; vec[1] += y; vec[2] += z;
            }
    
            // Basic operations
            Vector3D operator+(const Vector3D& other) const {
                return Vector3D(vec[0] + other.vec[0], vec[1] + other.vec[1], vec[2] + other.vec[2]);
            }
    
            Vector3D operator-(const Vector3D& other) const {
                return Vector3D(vec[0] - other.vec[0], vec[1] - other.vec[1], vec[2] - other.vec[2]);
            }
    
            Vector3D& operator+=(const Vector3D& other) {
                vec[0] += other.vec[0];
                vec[1] += other.vec[1];
                vec[2] += other.vec[2];
                return *this;
            }
    
            Vector3D& operator-=(const Vector3D& other) {
                vec[0] -= other.vec[0];
                vec[1] -= other.vec[1];
                vec[2] -= other.vec[2];
                return *this;
            }
    
            // Scalar operations
            Vector3D operator*(float scalar) const {
                return Vector3D(vec[0] * scalar, vec[1] * scalar, vec[2] * scalar);
            }

			Vector3D operator*(const Vector3D& other) const {
                return Vector3D(vec[0] * other.GetX(), vec[1] * other.GetY(), vec[2] * other.GetZ());
            }
    
            Vector3D operator/(float scalar) const {
                if (scalar == 0) throw std::invalid_argument("Division by zero");
                return Vector3D(vec[0] / scalar, vec[1] / scalar, vec[2] / scalar);
            }
    
            // Vector operations
            float Dot(const Vector3D& other) const {
                return vec[0] * other.vec[0] + vec[1] * other.vec[1] + vec[2] * other.vec[2];
            }
    
            Vector3D Cross(const Vector3D& other) const {
                return Vector3D(
                    vec[1] * other.vec[2] - vec[2] * other.vec[1],
                    vec[2] * other.vec[0] - vec[0] * other.vec[2],
                    vec[0] * other.vec[1] - vec[1] * other.vec[0]
                );
            }
    
            // Magnitude and normalization
            float Magnitude() const {
                return std::sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);
            }
    
            // Normalizes the current vector
            Vector3D& Normalize() {
                float mag = Magnitude();

                if(mag == 0) {
                    vec[0] = 0;
                    vec[1] = 0;
                    vec[2] = 0;
                } 
                else 
                {
                    vec[0] /= mag;
                    vec[1] /= mag;
                    vec[2] /= mag;
                }

				return *this;
            }
    
            // Makes another instance of this vector and noröalizes it
            Vector3D Normalized() const {
                Vector3D result = *this;
                result.Normalize();
                return result;
            }
    
            // Equality comparison
            bool operator==(const Vector3D& other) const {
                return std::abs(vec[0] - other.vec[0]) < EPSILON &&
                       std::abs(vec[1] - other.vec[1]) < EPSILON &&
                       std::abs(vec[2] - other.vec[2]) < EPSILON;
            }
    
            bool operator!=(const Vector3D& other) const {
                return !(*this == other);
            }

            friend std::ostream& operator<< (std::ostream& os, const Vector3D& vec) {
                os << vec.ToString();
                return os;
            }

            Vector<float, 3> BaseVector() {
                return vec;
            }
    
            std::string ToString() const {
                return ToString(2);
            }

            std::string ToString(uint8_t precision) const {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(precision);
                ss << "Vector3D(" << vec[0] << ", " << vec[1] << ", " << vec[2] << ")";
                return ss.str();
            }

            float* ToArray() {
                return vec.ToRawArray();
            }

            static Vector3D Zero() { return Vector3D(0,0,0);}
            static Vector3D Identity() { return Vector3D(1,1,1);}
            static Vector3D Up() { return VECTOR_Up; }
            static Vector3D Down() { return VECTOR_Down; }
            static Vector3D Right() { return VECTOR_Right; }
            static Vector3D Left() { return VECTOR_Left; }
            static Vector3D Forward() { return VECTOR_Forward; }
            static Vector3D Backward() { return VECTOR_Backward; }

        private:
            Vector<float, 3> vec;
        };
    
        /// Four-component float vector for homogeneous coordinates, RGBA
        /// values, and shader constant payloads.
        ///
        /// Wraps Vector<float, 4> with the same accessor pattern as
        /// Vector2D and Vector3D, adding a W component. Reach for it when a
        /// value has to survive a Matrix4 transform with its translation
        /// intact (`w = 1` for points, `w = 0` for directions), or when you
        /// are packing four floats for the GPU.
        ///
        /// @code{.cpp}
        /// using Sleak::Math::Vector4D;
        ///
        /// Vector4D point(1.0f, 2.0f, 3.0f, 1.0f);      // a position
        /// Vector4D direction(0.0f, 1.0f, 0.0f, 0.0f);  // a direction
        /// Vector4D tint(1.0f, 0.95f, 0.85f, 1.0f);     // RGBA
        ///
        /// float alpha = tint.GetW();
        /// @endcode
        ///
        /// @see Vector2D, Vector3D, Vector, Matrix4, Color
        /// @ingroup math
        class Vector4D {
        public:
            // Constructors
            Vector4D() : vec({0.0f, 0.0f, 0.0f, 0.0f}) {}
            Vector4D(float x, float y, float z, float w) : vec({x, y, z, w}) {}
    
            // Accessors
            float GetX() const { return vec[0]; }
            float GetY() const { return vec[1]; }
            float GetZ() const { return vec[2]; }
            float GetW() const { return vec[3]; }
    
            // Mutators
            void SetX(float val) { vec[0] = val; }
            void SetY(float val) { vec[1] = val; }
            void SetZ(float val) { vec[2] = val; }
            void SetW(float val) { vec[3] = val; }
            void Set(float x, float y, float z, float w) {
                vec[0] = x; vec[1] = y; vec[2] = z; vec[3] = w;
            }
    
            // Basic operations
            Vector4D operator+(const Vector4D& other) const {
                return Vector4D(vec[0] + other.vec[0], vec[1] + other.vec[1], vec[2] + other.vec[2], vec[3] + other.vec[3]);
            }
    
            Vector4D operator-(const Vector4D& other) const {
                return Vector4D(vec[0] - other.vec[0], vec[1] - other.vec[1], vec[2] - other.vec[2], vec[3] - other.vec[3]);
            }
    
            Vector4D& operator+=(const Vector4D& other) {
                vec[0] += other.vec[0];
                vec[1] += other.vec[1];
                vec[2] += other.vec[2];
                vec[3] += other.vec[3];
                return *this;
            }
    
            Vector4D& operator-=(const Vector4D& other) {
                vec[0] -= other.vec[0];
                vec[1] -= other.vec[1];
                vec[2] -= other.vec[2];
                vec[3] -= other.vec[3];
                return *this;
            }
    
            // Scalar operations
            Vector4D operator*(float scalar) const {
                return Vector4D(vec[0] * scalar, vec[1] * scalar, vec[2] * scalar, vec[3] * scalar);
            }
    
            Vector4D operator/(float scalar) const {
                if (scalar == 0) throw std::invalid_argument("Division by zero");
                return Vector4D(vec[0] / scalar, vec[1] / scalar, vec[2] / scalar, vec[3] / scalar);
            }
    
            // Vector operations
            float Dot(const Vector4D& other) const {
                return vec[0] * other.vec[0] + vec[1] * other.vec[1] + vec[2] * other.vec[2] + vec[3] * other.vec[3];
            }
    
            // Magnitude and normalization
            float Magnitude() const {
                return std::sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2] + vec[3] * vec[3]);
            }
    
            void Normalize() {
                float mag = Magnitude();
                if (mag == 0) throw std::runtime_error("Cannot normalize a zero vector");
                vec[0] /= mag;
                vec[1] /= mag;
                vec[2] /= mag;
                vec[3] /= mag;
            }
    
            Vector4D Normalized() const {
                Vector4D result = *this;
                result.Normalize();
                return result;
            }
    
            // Equality comparison
            bool operator==(const Vector4D& other) const {
                return std::abs(vec[0] - other.vec[0]) < EPSILON &&
                       std::abs(vec[1] - other.vec[1]) < EPSILON &&
                       std::abs(vec[2] - other.vec[2]) < EPSILON &&
                       std::abs(vec[3] - other.vec[3]) < EPSILON;
            }
    
            bool operator!=(const Vector4D& other) const {
                return !(*this == other);
            }

            friend std::ostream& operator<< (std::ostream& os, const Vector4D& vec) {
                os << vec.ToString();
                return os;
            }

            std::string ToString() const {
                return ToString(2);
            }

            std::string ToString(uint8_t precision) const {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(precision);
                ss << "Vector4D(" << vec[0] << ", " << vec[1] << ", " << vec[2] << ", " << vec[3] << ")";
                return ss.str();
            }

            float* ToArray() {
                return vec.ToRawArray();
            }
    
        private:
            Vector<float, 4> vec;
        };
    
        // Free functions for scalar multiplication (commutative)
        template <typename T, size_t N>
        Vector<T, N> operator*(T scalar, const Vector<T, N>& vec) {
            return vec * scalar;
        }

    }
} // namespace Sleak


#endif