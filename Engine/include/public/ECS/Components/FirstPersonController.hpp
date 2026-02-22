#ifndef _FIRSTPERSONCONTROLLER_HPP_
#define _FIRSTPERSONCONTROLLER_HPP_

#include <ECS/Components/CameraController.hpp>
#include <Events/KeyboardEvent.h>

namespace Sleak {

    class RigidbodyComponent;

    // First person controller modeled after Unreal Engine's CharacterMovementComponent.
    // Uses acceleration/braking model with very low air control.
    class FirstPersonController : public CameraController {
    public:
        FirstPersonController(GameObject* object);

        bool Initialize() override;
        void Update(float deltaTime) override;
        void SetEnabled(bool enabled) override;

        void ToggleCursor(bool enable) override;

        void SetMaxWalkSpeed(float speed) { m_maxWalkSpeed = speed; }
        float GetMaxWalkSpeed() const { return m_maxWalkSpeed; }
        void SetSprintSpeedMultiplier(float mult) { m_sprintMultiplier = mult; }
        float GetSprintSpeedMultiplier() const { return m_sprintMultiplier; }
        void SetJumpZVelocity(float vel) { m_jumpZVelocity = vel; }
        float GetJumpZVelocity() const { return m_jumpZVelocity; }
        void SetMaxAcceleration(float accel) { m_maxAcceleration = accel; }
        void SetBrakingDeceleration(float decel) { m_brakingDeceleration = decel; }
        void SetGroundFriction(float friction) { m_groundFriction = friction; }

        float GetPitch() const { return m_pitch; }
        float GetYaw() const { return m_yaw; }

        void OnKeyPressed(const Events::Input::KeyPressedEvent& e);
        void OnKeyReleased(const Events::Input::KeyReleasedEvent& e);

    private:
        void UpdateInput(float deltaTime) override;
        void UpdateCamera(float deltaTime) override;

        // UE-style movement parameters — tuned slower for a calmer pace
        float m_maxWalkSpeed = 2.5f;             // Casual walking pace
        float m_sprintMultiplier = 2.0f;         // Sprint speed = walk * multiplier
        bool m_sprinting = false;
        float m_maxAcceleration = 10.0f;         // Gentle ramp-up (~0.25s to full speed)
        float m_brakingDeceleration = 12.0f;     // Smooth stop
        float m_groundFriction = 8.0f;           // UE default
        float m_airControl = 0.05f;              // Minimal air control

        // Jump — gentle hop, ~0.45m height
        // v = sqrt(2 * 9.81 * 0.45) ≈ 2.97
        float m_jumpZVelocity = 3.0f;

        // Mouse look
        float m_pitch = 0.0f;
        float m_yaw = 0.0f;
        Math::Vector2D m_pitchRange = Math::Vector2D(-89, 89);

        // Horizontal velocity (managed by controller, separate from rigidbody Y axis)
        Math::Vector3D m_velocity;

        // Input state
        bool m_firstFrame = true;

        RigidbodyComponent* m_rigidbody = nullptr;
    };

} // namespace Sleak

#endif // _FIRSTPERSONCONTROLLER_HPP_
