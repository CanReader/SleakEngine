#include <Math/Quaternion.hpp>
#include <cmath>
#include <Math/Matrix.hpp>

namespace Sleak {
    namespace Math {

        // Constructors
        Quaternion::Quaternion() : w(1.0f), x(0.0f), y(0.0f), z(0.0f) {}

        Quaternion::Quaternion(float w, float x, float y, float z)
            : w(w), x(x), y(y), z(z) {}

        Quaternion::Quaternion(const Vector3D& axis, float angle) {
            float halfAngle = angle / 2.0f;
            float sinHalfAngle = sin(halfAngle);
            axis.Normalized();
            w = cos(halfAngle);
            x = axis.GetX() * sinHalfAngle;
            y = axis.GetY() * sinHalfAngle;
            z = axis.GetZ() * sinHalfAngle;
        }

        Quaternion::Quaternion(const Matrix4& matrix) {
            float trace = matrix(0, 0) + matrix(1, 1) + matrix(2, 2);
            if (trace > 0) {
                float s = 0.5f / sqrt(trace + 1.0f);
                w = 0.25f / s;
                x = (matrix(2, 1) - matrix(1, 2)) * s;
                y = (matrix(0, 2) - matrix(2, 0)) * s;
                z = (matrix(1, 0) - matrix(0, 1)) * s;
            } else {
                if (matrix(0, 0) > matrix(1, 1) && matrix(0, 0) > matrix(2, 2)) {
                    float s = 2.0f * sqrt(1.0f + matrix(0, 0) - matrix(1, 1) - matrix(2, 2));
                    w = (matrix(2, 1) - matrix(1, 2)) / s;
                    x = 0.25f * s;
                    y = (matrix(0, 1) + matrix(1, 0)) / s;
                    z = (matrix(0, 2) + matrix(2, 0)) / s;
                } else if (matrix(1, 1) > matrix(2, 2)) {
                    float s = 2.0f * sqrt(1.0f + matrix(1, 1) - matrix(0, 0) - matrix(2, 2));
                    w = (matrix(0, 2) - matrix(2, 0)) / s;
                    x = (matrix(0, 1) + matrix(1, 0)) / s;
                    y = 0.25f * s;
                    z = (matrix(1, 2) + matrix(2, 1)) / s;
                } else {
                    float s = 2.0f * sqrt(1.0f + matrix(2, 2) - matrix(0, 0) - matrix(1, 1));
                    w = (matrix(1, 0) - matrix(0, 1)) / s;
                    x = (matrix(0, 2) + matrix(2, 0)) / s;
                    y = (matrix(1, 2) + matrix(2, 1)) / s;
                    z = 0.25f * s;
                }
            }
        }

        // Accessors and Mutators
        float& Quaternion::operator[](int index) {
            switch (index) {
                case 0: return w;
                case 1: return x;
                case 2: return y;
                case 3: return z;
                default: throw IndexOutOfBoundsException("Quaternion index out of range");
            }
        }

        const float& Quaternion::operator[](int index) const {
            switch (index) {
                case 0: return w;
                case 1: return x;
                case 2: return y;
                case 3: return z;
                default: throw IndexOutOfBoundsException("Quaternion index out of range");
            }
        }

        // Quaternion Operations
        Quaternion Quaternion::operator*(const Quaternion& other) const {
            return Quaternion(
                w * other.w - x * other.x - y * other.y - z * other.z,
                w * other.x + x * other.w + y * other.z - z * other.y,
                w * other.y - x * other.z + y * other.w + z * other.x,
                w * other.z + x * other.y - y * other.x + z * other.w
            );
        }

        Quaternion& Quaternion::operator*=(const Quaternion& other) {
            *this = *this * other;
            return *this;
        }

        Quaternion Quaternion::operator-() const {
            return Quaternion(-w, -x, -y, -z);
        }

        float Quaternion::GetX() const { return x; }
        float Quaternion::GetY() const { return y; }
        float Quaternion::GetZ() const { return z; }
        float Quaternion::GetW() const { return w; }

        // Normalize the quaternion
        void Quaternion::normalize() {
            float magnitude = sqrt(w * w + x * x + y * y + z * z);
            if (magnitude > 0.0f) {
                w /= magnitude;
                x /= magnitude;
                y /= magnitude;
                z /= magnitude;
            }
        }

        // Calculate the magnitude of the quaternion
        float Quaternion::magnitude() const {
            return sqrt(w * w + x * x + y * y + z * z);
        }

        // Calculate the conjugate of the quaternion
        Quaternion Quaternion::conjugate() const {
            return Quaternion(w, -x, -y, -z);
        }

        // Calculate the inverse of the quaternion
        Quaternion Quaternion::inverse() const {
            float magSq = w * w + x * x + y * y + z * z;
            if (magSq > 0.0f) {
                float invMagSq = 1.0f / magSq;
                return Quaternion(w * invMagSq, -x * invMagSq, -y * invMagSq, -z * invMagSq);
            }
            return Quaternion();
        }

        // Rotate a vector by this quaternion
        Vector3D Quaternion::rotateVector(const Vector3D& vec) const {
            Quaternion qVec(0.0f, vec.GetX(), vec.GetY(), vec.GetZ());
            Quaternion result = *this * qVec * inverse();
            return Vector3D(result.x, result.y, result.z);
        }

        // Conversion to Rotation Matrix
        Matrix4 Quaternion::toRotationMatrix() const {
            Matrix4 matrix;
            matrix(0, 0) = 1.0f - 2.0f * y * y - 2.0f * z * z;
            matrix(0, 1) = 2.0f * x * y - 2.0f * w * z;
            matrix(0, 2) = 2.0f * x * z + 2.0f * w * y;
            matrix(0, 3) = 0.0f;

            matrix(1, 0) = 2.0f * x * y + 2.0f * w * z;
            matrix(1, 1) = 1.0f - 2.0f * x * x - 2.0f * z * z;
            matrix(1, 2) = 2.0f * y * z - 2.0f * w * x;
            matrix(1, 3) = 0.0f;

            matrix(2, 0) = 2.0f * x * z - 2.0f * w * y;
            matrix(2, 1) = 2.0f * y * z + 2.0f * w * x;
            matrix(2, 2) = 1.0f - 2.0f * x * x - 2.0f * y * y;
            matrix(2, 3) = 0.0f;

            matrix(3, 0) = 0.0f;
            matrix(3, 1) = 0.0f;
            matrix(3, 2) = 0.0f;
            matrix(3, 3) = 1.0f;

            return matrix;
        }

        // Static method for look rotation
        Quaternion Quaternion::LookRotation(Vector3D& forward, Vector3D& up) {
            Vector3D f = forward.Normalize();
            Vector3D r = f.Cross(up).Normalize();
            Vector3D u = r.Cross(f);
            u.Normalize();

            Matrix4 rotationMatrix;
            rotationMatrix(0, 0) = r.GetX(); rotationMatrix(0, 1) = r.GetY(); rotationMatrix(0, 2) = r.GetZ(); rotationMatrix(0, 3) = 0.0f;
            rotationMatrix(1, 0) = u.GetX(); rotationMatrix(1, 1) = u.GetY(); rotationMatrix(1, 2) = u.GetZ(); rotationMatrix(1, 3) = 0.0f;
            rotationMatrix(2, 0) = -f.GetX(); rotationMatrix(2, 1) = -f.GetY(); rotationMatrix(2, 2) = -f.GetZ(); rotationMatrix(2, 3) = 0.0f;
            rotationMatrix(3, 0) = 0.0f; rotationMatrix(3, 1) = 0.0f; rotationMatrix(3, 2) = 0.0f; rotationMatrix(3, 3) = 1.0f;

            return Quaternion(rotationMatrix);
        }
        
        // Free function for quaternion-vector multiplication
        Vector3D operator*(const Quaternion& quat, const Vector3D& vec) {
            Quaternion vecQuat(0, vec.GetX(), vec.GetY(), vec.GetZ());
            Quaternion resultQuat = quat * vecQuat * quat.conjugate();
            return Vector3D(resultQuat.x, resultQuat.y, resultQuat.z);
        }

    } // namespace Math
} // namespace Sleak