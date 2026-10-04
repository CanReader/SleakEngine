#ifndef _CAMERA_HPP_
#define _CAMERA_HPP_

#include <Utility/Exception.hpp>
#include <Core/GameObject.hpp>
#include <Camera/RenderView.hpp>
#include <Camera/ViewFrustum.hpp>
#include <Math/Matrix.hpp>
#include <Math/Vector.hpp>

namespace Sleak {
    /// Perspective or orthographic projection mode for a Camera.
    /// @ingroup camera
    enum class ProjectionType {Perspective, Orthographic};

    /// GameObject that describes a view: position, look target, and
    /// projection. Position/orientation come from CameraController subclasses.
    ///
    /// Create one, add it to the scene, and call
    /// SceneBase::SetActiveCamera() so the renderer knows which view to
    /// draw from. Because Camera is a GameObject, it also carries
    /// components: attach a FreeLookCameraController for a debug or
    /// spectator view, or a FirstPersonController plus a
    /// ColliderComponent and RigidbodyComponent for a player character
    /// that collides with the world.
    ///
    /// Each camera keeps its own view and projection matrices. Only the
    /// scene's active camera drives rendering: after the update pass the
    /// scene turns it into a RenderView through BuildRenderView() and
    /// submits the frame with that. Other cameras in the scene can move
    /// freely without affecting what is drawn or culled.
    ///
    /// Orientation is expressed as a look target rather than a rotation.
    /// SetLookTarget() aims at a world point and SetDirection() aims along
    /// a vector; GetDirection() returns the normalized facing.
    ///
    /// @code{.cpp}
    /// auto* cam = new Sleak::Camera(
    ///     "MainCamera",
    ///     Sleak::Math::Vector3D(-5.0f, 3.0f, -5.0f),
    ///     /*fov=*/60.0f, /*near=*/0.01f, /*far=*/200.0f);
    ///
    /// cam->SetLookTarget(Sleak::Math::Vector3D(0.0f, 0.0f, 0.0f));
    /// cam->AddComponent<Sleak::FreeLookCameraController>();
    /// cam->Initialize();
    ///
    /// if (auto* ctrl =
    ///         cam->GetComponent<Sleak::FreeLookCameraController>()) {
    ///     ctrl->SetEnabled(true);
    /// }
    ///
    /// AddObject(cam);
    /// SetActiveCamera(cam);
    /// @endcode
    ///
    /// @see CameraController, FreeLookCameraController,
    ///      FirstPersonController, ViewFrustum, CullingSystem
    /// @ingroup camera
    class ENGINE_API Camera : public GameObject {
    public:
        Camera(std::string name = "Camera",
               Math::Vector3D Position = Math::Vector3D(0, 0, -3.5),
               float fov = 60, float near = 1.0f, float far = 1000.0f);

        void Initialize() override;
        /// Recalculates this camera's view/projection matrices while active.
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
        
        /// Updates the viewport dimensions and recomputes the projection matrix.
        void OnResize(uint32_t width, uint32_t height);

        const Math::Matrix4& GetViewMatrix() const { return m_view; }
        const Math::Matrix4& GetProjectionMatrix() const {
            return m_projection;
        }

        /// Builds a RenderView from the camera's current position, target
        /// and projection.
        RenderView BuildRenderView() const;

        /// @deprecated Use RenderView::GetCurrent().view.
        static const Math::Matrix4& GetMainViewMatrix() {
            return RenderView::GetCurrent().view;
        }

        /// @deprecated Use RenderView::GetCurrent().projection.
        static const Math::Matrix4& GetMainProjectionMatrix() {
            return RenderView::GetCurrent().projection;
        }

        /// @deprecated Use RenderView::GetCurrent().position.
        static const Math::Vector3D& GetMainCameraPosition() {
            return RenderView::GetCurrent().position;
        }

        /// @deprecated Use RenderView::GetCurrent().frustum.
        static const ViewFrustum& GetMainViewFrustum() {
            return RenderView::GetCurrent().frustum;
        }

    protected:
        /// Rebuilds this camera's view matrix from position/target/up.
        void RecalculateViewMatrix();
        /// Rebuilds this camera's projection matrix from FOV/aspect/near/far
        /// or the orthographic extents.
        void RecalculateProjectionMatrix();

        /// LookAt matrix for the current position, target and up vector.
        Math::Matrix4 ComputeViewMatrix() const;
        /// Perspective or orthographic matrix for the current settings.
        Math::Matrix4 ComputeProjectionMatrix() const;

        float fieldOfView;
        float nearPlane;
        float farPlane;
        float width;
        float height;

        ProjectionType type = ProjectionType::Perspective;

        Math::Vector3D Position;
        Math::Vector3D LookTarget;
        Math::Vector3D Up = Math::Vector3D::Up();

        Math::Matrix4 m_view = Math::Matrix4::Identity();
        Math::Matrix4 m_projection = Math::Matrix4::Identity();
    };
}

#endif