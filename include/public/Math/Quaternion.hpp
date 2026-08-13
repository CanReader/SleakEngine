#ifndef _QUATERNION_H_
#define _QUATERNION_H_

#include <Math/Vector.hpp>
#include <Utility/Exception.hpp>

namespace Sleak {
    namespace Math {

        /**
         * @class Quaternion
         * @brief Represents a quaternion for 3D rotations.
         *
         * Quaternions provide a more robust and efficient way to represent rotations
         * compared to Euler angles, avoiding issues like gimbal lock.
         *
         * This class provides functionalities for creating, manipulating, and
         * using quaternions for various 3D rotation operations.
         */
        
         //Forward Declare
         template <typename T, size_t Rows, size_t Cols>
        class Matrix;

        class Quaternion {
        public:
            float w; // Scalar component
            float x; // Vector component (i)
            float y; // Vector component (j)
            float z; // Vector component (k)

            // Constructors
            Quaternion();
            Quaternion(float w, float x, float y, float z);
            Quaternion(const Vector3D& axis, float angle);
            Quaternion(const Matrix<float, 4, 4>& matrix);

            // Accessors and Mutators
            float& operator[](int index);
            const float& operator[](int index) const;

            // Quaternion Operations
            Quaternion operator*(const Quaternion& other) const;
            Quaternion& operator*=(const Quaternion& other);
            Quaternion operator-() const;

            float GetX() const;
            float GetY() const;
            float GetZ() const;
            float GetW() const;

            // Normalize the quaternion
            void normalize();

            // Calculate the magnitude of the quaternion
            float magnitude() const;

            // Calculate the conjugate of the quaternion
            Quaternion conjugate() const;

            // Calculate the inverse of the quaternion
            Quaternion inverse() const;

            // Rotate a vector by this quaternion
            Vector3D rotateVector(const Vector3D& vec) const;

            // Conversion to Rotation Matrix
            Matrix<float, 4, 4> toRotationMatrix() const;

            // Static method for look rotation
            static Quaternion LookRotation(Vector3D& forward, Vector3D& up);
        private:
        };

        // Free function for quaternion-vector multiplication
        Vector3D operator*(const Quaternion& quat, const Vector3D& vec);

    } // namespace Math
} // namespace Sleak

#endif // _QUATERNION_H_