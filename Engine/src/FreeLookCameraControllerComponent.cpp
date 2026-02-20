#include <ECS/Components/FreeLookCameraController.hpp>
#include <Physics/RigidbodyComponent.hpp>
#include <Core/Application.hpp>
#include <Window.hpp>
#include <SDL3/SDL.h>
#include <Events/Event.h>
#include <Math/Math.hpp>
#include <Math/Quaternion.hpp>

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

        EventDispatcher::RegisterEventHandler(this, &FreeLookCameraController::OnKeyPressed);
        EventDispatcher::RegisterEventHandler(this, &FreeLookCameraController::OnKeyReleased);

    }

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
        bool cursor_set = enabled ? SDL_HideCursor() : SDL_ShowCursor();

        if(!cursor_set) {
            SLEAK_ERROR("Failed to set cursor visibility {}", SDL_GetError());
        }
    }

    void FreeLookCameraController::UpdateInput(float deltaTime) {
        // TODO: Make a class to handle keyboard LATER
        // TODO: Make a class to handle mouse inputs LATER

        float x, y;
        SDL_GetRelativeMouseState(&x, &y);

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
        Math::Vector3D targetVelocity(
            translationInput.GetX() * speed,   // Right/Left
            translationInput.GetY() * speed,   // Up/Down
            translationInput.GetZ() * speed    // Forward/Backward
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

    void FreeLookCameraController::OnKeyPressed(const Sleak::Events::Input::KeyPressedEvent& e) {
        switch (e.GetKeyCode()) {
            case Input::KEY_CODE::KEY__W: translationInput.SetZ(1.0f); break;
            case Input::KEY_CODE::KEY__S: translationInput.SetZ(-1.0f); break;
            case Input::KEY_CODE::KEY__A: translationInput.SetX(+1.0f); break;
            case Input::KEY_CODE::KEY__D: translationInput.SetX(-1.0f); break;
            case Input::KEY_CODE::KEY__SPACE: translationInput.SetY(1.0f); break;
            case Input::KEY_CODE::KEY__LSHIFT: translationInput.SetY(-1.0f); break;
            case Input::KEY_CODE::KEY__LCTRL: if(!e.IsRepeat()) speed *= 2; break;
        }
    }

    void FreeLookCameraController::OnKeyReleased(const Sleak::Events::Input::KeyReleasedEvent& e) {
        switch (e.GetKeyCode()) {
            case Input::KEY_CODE::KEY__W: 
            case Input::KEY_CODE::KEY__S: translationInput.SetZ(0.0f); break;
            case Input::KEY_CODE::KEY__A: 
            case Input::KEY_CODE::KEY__D: translationInput.SetX(0.0f); break;
            case Input::KEY_CODE::KEY__SPACE: 
            case Input::KEY_CODE::KEY__LSHIFT: translationInput.SetY(0.0f); break;
            case Input::KEY_CODE::KEY__LCTRL: speed /= 2; break;
        }
    }

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