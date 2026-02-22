#ifndef _CAMERA_HPP_
#define _CAMERA_HPP_

#include <Utility/Exception.hpp>
#include <Core/GameObject.hpp>
#include <Camera/ViewFrustum.hpp>
#include <Math/Matrix.hpp>
#include <Math/Vector.hpp>

namespace Sleak {
    enum class ProjectionType {Perspective, Orthographic};
    class Camera : public GameObject {
    public:
        Camera(std::string name = "Camera",
               Math::Vector3D Position = Math::Vector3D(0, 0, -3.5),
               float fov = 60, float near = 1.0f, float far = 1000.0f);

        void Initialize() override;
        void Update(float DeltaTime) override;

        void SetFieldOfView(float fov) {fieldOfView = fov;}
        float GetFieldOfView() const {return fieldOfView;}

        void SetNearPlane(float nearPlaneValue) {nearPlane = nearPlaneValue;}
        float GetNearPlane() const {return nearPlane;}
        
        void SetFarPlane(float farPlaneValue) {farPlane = farPlaneValue;}
        float GetFarPlane() const {return farPlane;}
        
        void SetPosition(const Math::Vector3D& position) {Position = position;}
        void AddPosition(const Math::Vector3D& position) {Position += position;}
        Math::Vector3D GetPosition() const {return Position;}

        Math::Vector3D GetDirection() const { return (LookTarget - Position).Normalized(); }
        void SetDirection(const Math::Vector3D& direction) {
            LookTarget = Position + direction.Normalized(); 
        }
        void AddDirection(const Math::Vector3D& direction) {
            LookTarget += Position + direction.Normalized();
        }

        void SetLookTarget(const Math::Vector3D& target) {LookTarget = target;}
        void AddLookTarget(const Math::Vector3D& target) { LookTarget += target; }
        Math::Vector3D GetLookTarget() const {return LookTarget;}
        
        void SetUp(const Math::Vector3D& up) {Up = up;}
        Math::Vector3D GetUp() const {return Up;}
        
        void SetProjectionType(ProjectionType type) {this->type = type;}
        ProjectionType GetProjectionType() const {return type;}
        
        void OnResize(uint32_t width, uint32_t height);

        static const Math::Matrix4& GetMainViewMatrix() {
            return Camera::View;
        }

        static const Math::Matrix4& GetMainProjectionMatrix() {
            return Camera::Projection;
        }

        static const Math::Vector3D& GetMainCameraPosition() {
            return Camera::MainPosition;
        }

        static const ViewFrustum& GetMainViewFrustum() {
            return Camera::s_frustum;
        }

    protected:
        void RecalculateViewMatrix();
        void RecalculateProjectionMatrix();

        float fieldOfView;
        float nearPlane;
        float farPlane;
        float width;
        float height;

        ProjectionType type = ProjectionType::Perspective;

        Math::Vector3D Position;
        Math::Vector3D LookTarget;
        Math::Vector3D Up = Math::Vector3D::Up();
        
        static Math::Matrix4 View;
        static Math::Matrix4 Projection;
        static Math::Vector3D MainPosition;
        static ViewFrustum s_frustum;
    };
}

#endif