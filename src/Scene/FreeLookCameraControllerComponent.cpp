#include <Core/Application.hpp>
#include <Core/Window.hpp>
#include <ECS/Components/FreeLookCameraController.hpp>
#include <Input/Keyboard.hpp>
#include <Input/Mouse.hpp>
#include <Math/Math.hpp>
#include <Math/Quaternion.hpp>
#include <Physics/RigidbodyComponent.hpp>

namespace Sleak {
 
    FreeLookCameraController::FreeLookCameraController(GameObject* object) 
        : CameraController(object) {

        speed = 5.0f;
        sensitivity = 0.01f;
        acceleration = 13.5f;
        damping = 8.0f;
        maxSpeed = 10.0f;

        YawRange = Math::Vector2D(-360, 360);
        PitchRange = Math::Vector2D(-89, 89);
        RollRange = Math::Vector2D(-89, 89);

        translationInput = Math::Vector3D::Zero();
        velocity = Math::Vector3D::Zero();
    }

    FreeLookCameraController::~FreeLookCameraController() = default;

    bool FreeLookCameraController::Initialize() {
        if (!CameraController::Initialize())
            return false;

        if (camera) {
            Math::Vector3D forward = camera->GetDirection();
            yaw = atan2(forward.GetX(), forward.GetZ());
            pitch = -asin(forward.GetY());
            pitch = Math::Clamp(pitch,
                static_cast<float>(PitchRange.GetX() * D2R),
                static_cast<float>(PitchRange.GetY() * D2R));
        }

        return true;
    }

    void FreeLookCameraController::Update(float deltaTime) {
        if (!bIsInitialized || !isEnabled) return;

        UpdateInput(deltaTime);
        UpdateCamera(deltaTime);
    }

    void FreeLookCameraController::ToggleCursor(bool enabled) {
        Input::Mouse::SetCursorVisible(enabled);
    }

    void FreeLookCameraController::UpdateInput(float deltaTime) {
        using Input::KEY_CODE;
        using Input::Keyboard;

        auto axis = [](KEY_CODE positive, KEY_CODE negative) {
            return (Keyboard::IsKeyHold(positive) ? 1.0f : 0.0f) -
                   (Keyboard::IsKeyHold(negative) ? 1.0f : 0.0f);
        };
        translationInput.SetZ(axis(KEY_CODE::KEY__W, KEY_CODE::KEY__S));
        translationInput.SetX(axis(KEY_CODE::KEY__A, KEY_CODE::KEY__D));
        translationInput.SetY(
            axis(KEY_CODE::KEY__SPACE, KEY_CODE::KEY__LSHIFT));
        m_boost = Keyboard::IsKeyHold(KEY_CODE::KEY__LCTRL);

        Math::Vector2D delta = Input::Mouse::GetDelta();
        float x = delta.GetX();
        float y = delta.GetY();

        if (m_firstFrame) {
            m_firstFrame = false;
            return;
        }

        // Smooth mouse movement using lerp
        MousePosition = Math::Lerp(MousePosition, Math::Vector2D(x, y), 0.2f);

        // Apply mouse sensitivity
        yaw += MousePosition.GetX() * sensitivity;
        pitch += MousePosition.GetY() * sensitivity * (isInvertY ? -1.0f : 1.0f);

        // Clamp pitch to prevent gimbal lock
        pitch = Math::Clamp(pitch, static_cast<float>(PitchRange.GetX() * D2R), static_cast<float>(PitchRange.GetY() * D2R));
    }
    
    void FreeLookCameraController::UpdateCamera(float deltaTime) {
        if(!camera)
            return;
        
        // Calculate rotation using clamped pitch and yaw
        Math::Quaternion yawRotation = Math::Quaternion(Math::Vector3D::Up(), yaw);
        Math::Quaternion pitchRotation = Math::Quaternion(Math::Vector3D::Right(), pitch);
        Math::Quaternion combinedRotation = yawRotation * pitchRotation;

        // Calculate camera axes
        Math::Vector3D forward = combinedRotation * Math::Vector3D::Forward();
        Math::Vector3D up = Math::Vector3D::Up();
        Math::Vector3D right = forward.Cross(up).Normalized();

        // Calculate acceleration based on input
        float currentSpeed = m_boost ? speed * 2.0f : speed;
        Math::Vector3D targetVelocity(
            translationInput.GetX() * currentSpeed,  // Right/Left
            translationInput.GetY() * currentSpeed,  // Up/Down
            translationInput.GetZ() * currentSpeed   // Forward/Backward
        );

        // Smooth velocity interpolation
        velocity = Math::Lerp(velocity, targetVelocity, deltaTime * acceleration);

        // Project velocity onto camera's local axes
        Math::Vector3D worldVelocity = 
            (right * velocity.GetX()) +
            (up * velocity.GetY()) +
            (forward * velocity.GetZ());

        // Clamp total velocity magnitude
        if (worldVelocity.Magnitude() > maxSpeed) {
            worldVelocity = worldVelocity.Normalized() * maxSpeed;
        }

        // Apply damping more consistently
        velocity = velocity * (1.0f - damping * deltaTime);

        // Cancel velocity component along collision normal for smooth sliding
        auto* rb = owner->GetComponent<RigidbodyComponent>();
        if (rb && rb->HadCollision()) {
            Math::Vector3D normal = rb->GetLastCollisionNormal();
            float dot = worldVelocity.Dot(normal);
            if (dot < 0.0f) {
                worldVelocity = worldVelocity - normal * dot;
            }
        }

        // Update camera position (collision handled by ColliderComponent + RigidbodyComponent)
        camera->AddPosition(worldVelocity * deltaTime);

        // Update look target
        Math::Vector3D lookTarget = camera->GetPosition() + forward;
        camera->SetLookTarget(lookTarget);
    }

    void FreeLookCameraController::OnKeyPressed(
        const Sleak::Events::Input::KeyPressedEvent&) {}

    void FreeLookCameraController::OnKeyReleased(
        const Sleak::Events::Input::KeyReleasedEvent&) {}

    void FreeLookCameraController::ApplyDamping(float deltaTime) 
    { 
        if (translationInput.Magnitude() == 0) {
            velocity = velocity * (1.0f - damping * deltaTime);
            if (velocity.Magnitude() < 0.01f) {
                velocity = Math::Vector3D::Zero();
            }
        }
    }
    

    void FreeLookCameraController::ClampVelocity() {
        if(velocity.Magnitude() > maxSpeed) {
            velocity = velocity.Normalize() * maxSpeed;
        }
    }
    
    void FreeLookCameraController::SetEnabled(bool enabled) {
        CameraController::SetEnabled(enabled);
    
        if(!camera)
            return;

        auto* app = Application::GetInstance();
        if (app) app->GetWindow().SetRelativeMouseMode(enabled);
    
        ToggleCursor(!enabled);
    
        if (enabled) {
            // Reset velocity and input when enabling
            velocity = Math::Vector3D::Zero();
            translationInput = Math::Vector3D::Zero();
            m_firstFrame = true;

            // Reset yaw and pitch to match the current camera direction
            Math::Vector3D forward = camera->GetDirection();
            yaw = atan2(forward.GetX(), forward.GetZ());
            pitch = -asin(forward.GetY());
    
            // Clamp pitch to avoid gimbal lock
            pitch = Math::Clamp(pitch, static_cast<float>(PitchRange.GetX() * D2R), static_cast<float>(PitchRange.GetY() * D2R));
        }
    }

}