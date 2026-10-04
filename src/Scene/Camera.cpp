#include <Camera/Camera.hpp>
#include <Core/Window.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Math/Math.hpp>

namespace Sleak {

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

    RenderView Camera::BuildRenderView() const {
        return RenderView::Create(ComputeViewMatrix(),
                                  ComputeProjectionMatrix(), Position);
    }

    void Camera::RecalculateViewMatrix() { m_view = ComputeViewMatrix(); }

    void Camera::RecalculateProjectionMatrix() {
        m_projection = ComputeProjectionMatrix();
    }

    Math::Matrix4 Camera::ComputeViewMatrix() const {
        Math::Vector3D eye = Position, target = LookTarget, up = Up;
        return Matrix4::LookAt(eye.BaseVector(), target.BaseVector(),
                               up.BaseVector());
    }

    Math::Matrix4 Camera::ComputeProjectionMatrix() const {
        if (type == ProjectionType::Perspective)
            return Matrix4::Perspective(fieldOfView * D2R, width / height,
                                        nearPlane, farPlane);

        return Matrix4::Orthographic(-width / 2.0f, width / 2.0f,
                                     -height / 2.0f, height / 2.0f, nearPlane,
                                     farPlane);
    }
    }  // namespace Sleak
