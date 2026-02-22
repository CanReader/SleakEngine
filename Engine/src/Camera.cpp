#include <Camera/Camera.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Math/Math.hpp>
#include <Window.hpp>

namespace Sleak {

    Math::Matrix4 Camera::View = Math::Matrix4::Identity();
    Math::Matrix4 Camera::Projection = Math::Matrix4::Identity();
    Math::Vector3D Camera::MainPosition = Math::Vector3D(0, 0, 0);
    ViewFrustum Camera::s_frustum;

    Camera::Camera(std::string Name, Math::Vector3D Position, float Fov, float Near, float Far) : GameObject(Name) {
        this->Position = Position;
        fieldOfView = Fov;
        nearPlane = Near;
        farPlane = Far;

        width = static_cast<float>(Window::GetWidth());
        height = static_cast<float>(Window::GetHeight());
    }

    void Camera::Initialize() {
        GameObject::Initialize();

        RecalculateViewMatrix();
        RecalculateProjectionMatrix();
    }

    void Camera::Update(float DeltaTime) {
        if(!bIsInitialized)
            return;

        GameObject::Update(DeltaTime);

        if(IsActive())
        {
            RecalculateViewMatrix(); // TODO: Temporary, will be replaced with dirty flag later
            RecalculateProjectionMatrix();
        }

    }

    void Camera::OnResize(uint32_t width, uint32_t height ) {
        this->width = width;
        this->height = height;

        RecalculateProjectionMatrix();
    }

    void Camera::RecalculateViewMatrix() {
        MainPosition = Position;

        View = Matrix4::LookAt(Position.BaseVector(),   // Position
                               LookTarget.BaseVector(), // Target
                               Up.BaseVector());        // Up

        // Update view frustum from VP = View * Projection (row-vector convention)
        Math::Matrix4 VP = View * Projection;
        s_frustum.ExtractFromVP(VP);
    }

    void Camera::RecalculateProjectionMatrix() {
        if(type == ProjectionType::Perspective)
            Projection = Matrix4::Perspective(fieldOfView * D2R, // Fov
                                              width/height,      // Aspect Ratio
                                              nearPlane,         // Near Plane
                                              farPlane);         // Far Plane
        else
            Projection = Matrix4::Orthographic(-width / 2.0f, width / 2.0f, 
                                               -height / 2.0f, height / 2.0f, 
                                                nearPlane, farPlane);
    }
}